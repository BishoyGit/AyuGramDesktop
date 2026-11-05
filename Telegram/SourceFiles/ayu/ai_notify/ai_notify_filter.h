// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_message_reaction_id.h"
#include "data/data_msg_id.h"
#include "data/data_types.h"

#include <QtCore/QObject>
#include <QtCore/QRegularExpression>
#include <deque>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

class History;
class HistoryItem;
class QTimer;

namespace Data {
class ForumTopic;
class SavedSublist;
class Thread;
enum class ItemNotificationType;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Window::Notifications {
class Manager;
struct NotificationFields;
} // namespace Window::Notifications

namespace Ayu::AiNotify {

// What the cheap local pre-check in System::schedule() decides:
// Allow — stock behaviour (VIP / filter effectively off for this item).
// Block — suppress entirely, no sound, no toast.
// Ask   — needs an LLM verdict; sound + toast are deferred until it arrives.
enum class PreDecision {
	Allow,
	Block,
	Ask,
};

struct Decision {
	crl::time ts = 0;
	QString chat;
	QString verdict; // "shown" / "suppressed" / "error (fallback: ...)"
	QString reason;
	int ms = 0;
};

struct Counters {
	int shown = 0;
	int suppressed = 0;
	int errors = 0;
};

class Gate final : public QObject {
public:
	static Gate &Instance();

	// --- hooks into the notification system (all on the main thread) ---

	// Called from System::schedule() for every candidate notification.
	[[nodiscard]] PreDecision PreRegister(not_null<HistoryItem*> item);

	// Called from NativeManager::doShowNotification(). Returns true when
	// the gate took over (suppress or hold for the LLM) — the caller must
	// not show the toast. Returns false to show it immediately.
	bool GateToast(
		const Window::Notifications::Manager::NotificationFields &fields);

	// --- consistency: cancel pending decisions like the stock system does ---

	void CancelHistory(not_null<History*> history);
	void CancelTopic(not_null<Data::ForumTopic*> topic);
	void CancelSublist(not_null<Data::SavedSublist*> sublist);
	void CancelSession(not_null<Main::Session*> session);
	void CancelItem(not_null<HistoryItem*> item);
	void CancelAll();

	// --- deferred sound: System registers how to play an alert, the gate
	// fires it only when a held notification is allowed through ---
	using SoundPlayer = Fn<void(
		not_null<Data::Thread*>,
		not_null<HistoryItem*>)>;
	void SetSoundPlayer(SoundPlayer player);

	// --- observability / control (settings UI) ---

	[[nodiscard]] std::vector<Decision> recentDecisions() const {
		return { _decisions.begin(), _decisions.end() };
	}
	[[nodiscard]] Counters counters() const {
		return _counters;
	}
	void resetStats();

	void TestConnection(Fn<void(QString)> done);
	void TestVerdict(Fn<void(QString)> done);

private:
	Gate();
	~Gate();

	// One notification held for an LLM verdict. Stored value-typed so the
	// original message may vanish or the session die while we wait.
	struct Pending {
		Main::Session *session = nullptr;
		std::shared_ptr<bool> alive;
		FullMsgId fullId;
		PeerId historyPeerId = 0;
		MsgId topicRootId = 0;
		PeerId monoforumPeerId = 0;
		int forwardedCount = 0;
		PeerId reactionFromId = 0;
		Data::ReactionId reactionId;
		QByteArray pollVoteOption;
		std::optional<DocumentId> soundId;
		crl::time enqueued = 0;
	};

	// Identifies a Data::Thread by values (safe across lifetimes).
	struct ThreadKey {
		Main::Session *session = nullptr;
		PeerId peerId = 0;
		MsgId topicRootId = 0;
		PeerId monoforumPeerId = 0;

		[[nodiscard]] friend bool operator<(
				const ThreadKey &a,
				const ThreadKey &b) {
			return std::tie(
				a.session,
				a.peerId.value,
				a.topicRootId.bare,
				a.monoforumPeerId.value
			) < std::tie(
				b.session,
				b.peerId.value,
				b.topicRootId.bare,
				b.monoforumPeerId.value
			);
		}
	};

	[[nodiscard]] bool quietHoursActive() const;
	[[nodiscard]] bool overRateLimit();
	[[nodiscard]] PreDecision matchKeywords(not_null<HistoryItem*> item);
	void trackSession(not_null<Main::Session*> session);
	void flush();
	void resolve(
		const Pending &pending,
		bool show,
		const QString &reason,
		bool isError,
		int ms = 0);
	void reentryShow(const Pending &pending, not_null<HistoryItem*> item);
	void playDeferredSound(
		not_null<Data::Thread*> thread,
		not_null<HistoryItem*> item);
	void log(
		const QString &chat,
		const QString &verdict,
		const QString &reason,
		int ms = 0);

	std::unique_ptr<class AiClient> _client;
	std::vector<Pending> _pending;
	QTimer *_flushTimer = nullptr;

	// generation guard: verdicts from requests dispatched before the last
	// CancelAll() are ignored
	int _generation = 0;

	std::deque<crl::time> _shownTimes;
	base::flat_map<ThreadKey, crl::time> _lastSoundAt;
	base::flat_set<Main::Session*> _trackedSessions;
	base::flat_map<Main::Session*, std::shared_ptr<bool>> _sessionAlive;

	std::deque<Decision> _decisions;
	Counters _counters;

	SoundPlayer _soundPlayer;
	bool _reentry = false;

	// keyword rules cache
	QString _keywordRulesSource;
	struct KeywordRule {
		bool allow = false;
		QString text;
		std::unique_ptr<QRegularExpression> regex;
	};
	std::vector<KeywordRule> _keywordRules;
};

} // namespace Ayu::AiNotify
