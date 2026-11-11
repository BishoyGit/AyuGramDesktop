// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ai_notify/ai_notify_filter.h"

#include "ayu/ai_notify/ai_notify_client.h"
#include "ayu/ai_notify/ai_notify_payload.h"
#include "ayu/ayu_settings.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "window/notifications_manager.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTime>
#include <QtCore/QTimer>
#include <gsl/util>
#include <map>

namespace Ayu::AiNotify {
namespace {

constexpr auto kMaxPending = 30;
constexpr auto kMaxInFlight = 2;
constexpr auto kRateWindowMs = crl::time(3600 * 1000);
constexpr auto kSoundThrottleMs = crl::time(2000);
constexpr auto kLogLimit = 100;

[[nodiscard]] QString DescribeMediaKind(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return u"text"_q;
	} else if (media->photo()) {
		return item->groupId() ? u"album"_q : u"photo"_q;
	} else if (const auto document = media->document()) {
		return document->isVoiceMessage()
			? u"voice message"_q
			: document->isVideoMessage()
			? u"video message"_q
			: document->isVideoFile()
			? u"video"_q
			: document->isAnimation()
			? u"gif"_q
			: document->sticker()
			? u"sticker"_q
			: document->isAudioFile()
			? u"music"_q
			: u"file"_q;
	} else if (media->poll()) {
		return u"poll"_q;
	} else if (media->location()) {
		return u"location"_q;
	} else if (media->sharedContact()) {
		return u"contact"_q;
	}
	return u"text"_q;
}

// Pull the assistant text out of a chat-completions response.
[[nodiscard]] QString ExtractContent(const QByteArray &raw) {
	const auto doc = QJsonDocument::fromJson(raw);
	if (!doc.isObject()) {
		return {};
	}
	const auto choices = doc.object().value("choices");
	if (!choices.isArray()) {
		return {};
	}
	const auto first = choices.toArray().at(0);
	if (!first.isObject()) {
		return {};
	}
	const auto message = first.toObject().value("message");
	if (!message.isObject()) {
		return {};
	}
	const auto content = message.toObject().value("content");
	if (content.isString()) {
		return content.toString();
	} else if (content.isArray()) {
		// Some providers return content as an array of parts.
		for (const auto &part : content.toArray()) {
			const auto object = part.toObject();
			if (object.value("type").toString() == u"text"_q) {
				return object.value("text").toString();
			}
		}
	}
	return {};
}

[[nodiscard]] QString StripFences(QString text) {
	text = text.trimmed();
	if (text.startsWith(u"```")) {
		const auto firstNewline = text.indexOf('\n');
		if (firstNewline >= 0) {
			text = text.mid(firstNewline + 1);
		}
		if (text.endsWith(u"```")) {
			text.chop(3);
		}
		text = text.trimmed();
	}
	return text;
}

[[nodiscard]] std::map<int, std::pair<bool, QString>> ParseVerdicts(
		const QByteArray &raw) {
	auto result = std::map<int, std::pair<bool, QString>>();
	auto text = StripFences(ExtractContent(raw));
	if (text.isEmpty()) {
		return result;
	}

	const auto parseObject = [&](const QJsonObject &obj) {
		if (obj.contains(u"show"_q)) {
			result[obj.value(u"id"_q).toInt()] = {
				obj.value(u"show"_q).toBool(),
				obj.value(u"reason"_q).toString(),
			};
		}
	};

	if (text.startsWith('[')) {
		const auto doc = QJsonDocument::fromJson(text.toUtf8());
		if (doc.isArray()) {
			for (const auto &value : doc.array()) {
				parseObject(value.toObject());
			}
		}
		return result;
	}
	const auto start = int(text.indexOf('{'));
	if (start < 0) {
		return result;
	}
	const auto doc = QJsonDocument::fromJson(text.mid(start).toUtf8());
	if (!doc.isObject()) {
		return result;
	}
	const auto object = doc.object();
	const auto results = object.value(u"results"_q);
	if (results.isArray()) {
		for (const auto &value : results.toArray()) {
			parseObject(value.toObject());
		}
	} else {
		parseObject(object);
	}
	return result;
}

[[nodiscard]] QByteArray ComposeSingleRequest(
		const Payload &payload,
		const QString &customPrompt) {
	auto object = QJsonObject{
		{"id", payload.id},
		{"chatType", payload.chatType},
		{"chat", payload.chat},
		{"sender", payload.sender},
		{"media", payload.mediaKind
			+ (payload.mediaNote.isEmpty()
				? QString()
				: u": "_q + payload.mediaNote)},
	};
	if (!payload.topic.isEmpty()) {
		object[u"topic"_q] = payload.topic;
	}
	if (!payload.text.isEmpty()) {
		object[u"text"_q] = payload.text;
	}
	if (!payload.replyText.isEmpty()) {
		object[u"replyTo"_q] = payload.replyText;
	}
	if (!payload.linkPreview.isEmpty()) {
		object[u"linkPreview"_q] = payload.linkPreview;
	}
	if (payload.isMention) {
		object[u"isMention"_q] = true;
	}

	auto itemsJson = QJsonArray();
	itemsJson.push_back(object);

	auto userContent = QJsonArray();
	userContent.push_back(QJsonObject{
		{"type", "text"},
		{"text", u"Classify the notification by its \"id\".\n\n"_q
			+ QString::fromUtf8(
				QJsonDocument(itemsJson).toJson(QJsonDocument::Compact))},
	});

	const auto systemPrompt = customPrompt.isEmpty()
		? DefaultSystemPrompt()
		: customPrompt;
	auto body = QJsonObject{
		{"model", AyuSettings::getInstance().aiNotifySettings().model()},
		{"temperature", 0},
		{"max_tokens", 300},
		{"messages", QJsonArray{
			QJsonObject{{"role", "system"}, {"content", systemPrompt}},
			QJsonObject{{"role", "user"}, {"content", userContent}},
		}},
	};
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

} // namespace

Gate &Gate::Instance() {
	static Gate instance;
	return instance;
}

Gate::Gate() : QObject(nullptr) {
	_client = std::make_unique<AiClient>(this);
	_flushTimer = new QTimer(this);
	_flushTimer->setSingleShot(true);
	connect(_flushTimer, &QTimer::timeout, this, [=] {
		flush();
	});
}

Gate::~Gate() = default;

bool Gate::quietHoursActive() const {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	const auto now = QTime::currentTime();
	const auto minute = now.hour() * 60 + now.minute();
	const auto from = ai.quietFromMinutes();
	const auto to = ai.quietToMinutes();
	return (from <= to)
		? (minute >= from && minute < to)
		: (minute >= from || minute < to);
}

bool Gate::overRateLimit() {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	const auto now = crl::now();
	while (!_shownTimes.empty() && now - _shownTimes.front() > kRateWindowMs) {
		_shownTimes.pop_front();
	}
	return int(_shownTimes.size()) >= ai.maxPerHour();
}

PreDecision Gate::matchKeywords(not_null<HistoryItem*> item) {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	const auto source = ai.keywordRules();
	if (source.isEmpty()) {
		return PreDecision::Ask;
	}
	if (source != _keywordRulesSource) {
		_keywordRulesSource = source;
		_keywordRules.clear();
		const auto lines = source.split('\n');
		for (auto line : lines) {
			line = line.trimmed();
			if (line.isEmpty() || line.startsWith('#')) {
				continue;
			}
			auto rule = KeywordRule();
			if (line.startsWith('+')) {
				rule.allow = true;
				line = line.mid(1);
			} else if (line.startsWith('-')) {
				line = line.mid(1);
			} else {
				// No sign means: hide when it matches.
			}
			if (line.startsWith(u"re:", Qt::CaseInsensitive)) {
				line = line.mid(3);
				rule.regex = std::make_unique<QRegularExpression>(
					line,
					QRegularExpression::CaseInsensitiveOption);
				if (!rule.regex->isValid()) {
					continue;
				}
			} else {
				rule.text = line;
			}
			_keywordRules.push_back(std::move(rule));
		}
	}
	if (_keywordRules.empty()) {
		return PreDecision::Ask;
	}

	auto haystack = item->notificationText().text;
	if (haystack.isEmpty()) {
		haystack = DescribeMediaKind(item);
	} else {
		haystack += u"\n"_q + DescribeMediaKind(item);
	}
	for (const auto &rule : _keywordRules) {
		const auto matched = rule.regex
			? rule.regex->match(haystack).hasMatch()
			: haystack.contains(rule.text, Qt::CaseInsensitive);
		if (!matched) {
			continue;
		}
		return rule.allow ? PreDecision::Allow : PreDecision::Block;
	}
	return PreDecision::Ask;
}

PreDecision Gate::PreRegister(not_null<HistoryItem*> item) {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	if (!ai.enabled()) {
		return PreDecision::Allow;
	}
	const auto peerId = item->history()->peer->id.value;
	if (ai.isVipId(peerId)) {
		return PreDecision::Allow;
	} else if (ai.isBlockedId(peerId)) {
		return PreDecision::Block;
	} else if (ai.quietHoursEnabled() && quietHoursActive()) {
		return PreDecision::Block;
	} else if (
		ai.onlyWhenUnfocused()
		&& Core::App().activeWindow() != nullptr) {
		return PreDecision::Block;
	} else if (matchKeywords(item) == PreDecision::Block) {
		return PreDecision::Block;
	} else if (overRateLimit()) {
		return PreDecision::Block;
	}
	return PreDecision::Ask;
}

void Gate::trackSession(not_null<Main::Session*> session) {
	if (!_trackedSessions.contains(session)) {
		_trackedSessions.insert(session);
		auto alive = std::make_shared<bool>(true);
		_sessionAlive[session] = alive;
		session->lifetime().add([this, session, alive] {
			*alive = false;
			_trackedSessions.remove(session);
			_sessionAlive.remove(session);
		});
	}
}

bool Gate::GateToast(
		const Window::Notifications::Manager::NotificationFields &fields) {
	if (_reentry) {
		return false;
	}
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	if (!ai.enabled()) {
		return false;
	}
	const auto item = fields.item;
	const auto peerId = item->history()->peer->id.value;
	const auto chat = ai.anonymizeNames()
		? u"chat #"_q + QString::number(peerId, 16)
		: item->history()->peer->name();

	if (ai.isVipId(peerId)) {
		++_counters.shown;
		log(chat, u"shown"_q, u"VIP chat"_q);
		return false;
	}

	const auto block = [&](const QString &reason) {
		++_counters.suppressed;
		log(chat, u"suppressed"_q, reason);
		return true;
	};
	if (ai.isBlockedId(peerId)) {
		return block(u"blocked chat"_q);
	} else if (ai.quietHoursEnabled() && quietHoursActive()) {
		return block(u"quiet hours"_q);
	} else if (
		ai.onlyWhenUnfocused()
		&& Core::App().activeWindow() != nullptr) {
		return block(u"window is focused"_q);
	} else if (overRateLimit()) {
		return block(u"rate limit"_q);
	}

	const auto keywords = matchKeywords(item);
	if (keywords == PreDecision::Allow) {
		++_counters.shown;
		log(chat, u"shown"_q, u"keyword rule"_q);
		return false;
	} else if (keywords == PreDecision::Block) {
		return block(u"keyword rule"_q);
	}

	// LLM path — hold the notification and decide asynchronously.
	if (int(_pending.size()) >= kMaxPending) {
		const auto oldest = _pending.front();
		_pending.erase(_pending.begin());
		resolve(
			oldest,
			ai.fallbackAction() == 0,
			u"queue overflow, applied fallback"_q,
			true);
	}

	auto pending = Pending{
		.session = &item->history()->session(),
		.alive = _sessionAlive.contains(&item->history()->session())
			? _sessionAlive[&item->history()->session()]
			: nullptr,
		.fullId = item->fullId(),
		.historyPeerId = peerId,
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = (item->history()->amMonoforumAdmin()
			? item->sublistPeerId()
			: PeerId()),
		.forwardedCount = fields.forwardedCount,
		.reactionFromId = fields.reactionFrom
			? fields.reactionFrom->id
			: PeerId(),
		.reactionId = fields.reactionId,
		.pollVoteOption = fields.pollVoteOption,
		.soundId = fields.soundId,
		.enqueued = crl::now(),
	};
	trackSession(pending.session);
	pending.alive = _sessionAlive[pending.session];
	_pending.push_back(std::move(pending));

	if (ai.batchWindowMs() <= 0) {
		flush();
	} else if (!_flushTimer->isActive()) {
		_flushTimer->start(ai.batchWindowMs());
	}
	return true;
}

void Gate::flush() {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	if (_pending.empty()) {
		return;
	} else if (_client->inFlightCount() >= kMaxInFlight) {
		_flushTimer->start(500);
		return;
	}

	auto batch = std::move(_pending);
	_pending.clear();

	struct Validated {
		Pending pending;
		Payload payload;
		QString chat;
		crl::time started = 0;
	};
	auto validated = std::make_shared<std::vector<Validated>>();
	for (auto &pending : batch) {
		if (!pending.alive || !*pending.alive || !pending.session) {
			continue;
		}
		const auto item = pending.session->data().message(pending.fullId);
		if (!item) {
			// The message vanished while we were deciding (deleted, chat
			// opened, account logged out) — nothing to show, nothing to say.
			continue;
		}
		validated->push_back({
			std::move(pending),
			BuildPayload(
				item,
				ai.anonymizeNames(),
				ai.sendText(),
				ai.textCap(),
				ai.sendImages(),
				ai.maxImages(),
				ai.imageMaxSide(),
				ai.fetchSmallImages()),
			item->history()->peer->name(),
			crl::now(),
		});
	}
	if (validated->empty()) {
		return;
	}
	for (auto i = 0; i < int(validated->size()); ++i) {
		(*validated)[i].payload.id = i + 1;
	}

	// Compose the OpenAI-compatible request.
	auto itemsJson = QJsonArray();
	auto imagesJson = QJsonArray();
	auto imageLegend = QStringList();
	auto imageIndex = 0;
	for (const auto &entry : *validated) {
		const auto &payload = entry.payload;
		auto object = QJsonObject{
			{"id", payload.id},
			{"chatType", payload.chatType},
			{"chat", payload.chat},
			{"sender", payload.sender},
			{"media", payload.mediaKind
				+ (payload.mediaNote.isEmpty()
					? QString()
					: u": "_q + payload.mediaNote)},
		};
		if (!payload.topic.isEmpty()) {
			object["topic"] = payload.topic;
		}
		if (!payload.text.isEmpty()) {
			object["text"] = payload.text;
		}
		if (!payload.replyText.isEmpty()) {
			object["replyTo"] = payload.replyText;
		}
		if (!payload.linkPreview.isEmpty()) {
			object["linkPreview"] = payload.linkPreview;
		}
		if (payload.isMention) {
			object["isMention"] = true;
		}
		for (const auto &image : payload.imagesJpeg) {
			++imageIndex;
			imageLegend.push_back(u"image %1 → id %2"_q
				.arg(imageIndex)
				.arg(payload.id));
			imagesJson.push_back(QJsonObject{
				{u"type"_q, u"image_url"_q},
				{u"image_url"_q, QJsonObject{
					{u"url"_q, u"data:image/jpeg;base64,"_q
						+ QString::fromLatin1(image.toBase64()),
				}},
			}});
		}
		itemsJson.push_back(object);
	}

	auto userContent = QJsonArray();
	const auto itemsText = QString::fromUtf8(
		QJsonDocument(itemsJson).toJson(QJsonDocument::Compact));
	const auto instructions = u"Classify each notification by its \"id\". "
		"For every id answer show=true only if it is worth interrupting the "
		"user right now."_q;
	auto textPart = QJsonObject{
		{"type", "text"},
		{"text", instructions
			+ (imageLegend.isEmpty()
				? QString()
				: u"\nAttached images in order: "_q + imageLegend.join(u", "_q))
			+ u"\n\n"_q
			+ itemsText},
	};
	userContent.push_back(textPart);
	for (const auto &image : imagesJson) {
		userContent.push_back(image);
	}

	const auto systemPrompt = ai.systemPrompt().isEmpty()
		? DefaultSystemPrompt()
		: ai.systemPrompt();

	auto body = QJsonObject{
		{"model", ai.model()},
		{"temperature", 0},
		{"max_tokens", 600},
		{"messages", QJsonArray{
			QJsonObject{{"role", "system"}, {"content", systemPrompt}},
			QJsonObject{{"role", "user"}, {"content", userContent}},
		}},
	};

	const auto started = crl::now();
	_client->Send(
		{
			.endpoint = ai.endpoint(),
			.apiKey = ai.apiKey(),
			.body = QJsonDocument(body).toJson(QJsonDocument::Compact),
			.timeoutMs = ai.timeoutMs(),
		},
		[this, validated, started, generation = _generation](
				const QByteArray &raw) {
			if (generation != _generation) {
				return;
			}
			auto verdicts = ParseVerdicts(raw);
			const auto latency = int(crl::now() - started);
			for (const auto &entry : *validated) {
				const auto it = verdicts.find(entry.payload.id);
				if (it == verdicts.end()) {
					resolve(
						entry.pending,
						AyuSettings::getInstance().aiNotifySettings()
							.fallbackAction() == 0,
						u"no verdict for id, applied fallback"_q,
						true,
						latency);
				} else {
					resolve(
						entry.pending,
						it->second.first,
						it->second.second,
						false,
						latency);
				}
			}
		},
		[this, validated, started, generation = _generation](QString error) {
			if (generation != _generation) {
				return;
			}
			const auto latency = int(crl::now() - started);
			for (const auto &entry : *validated) {
				resolve(
					entry.pending,
					AyuSettings::getInstance().aiNotifySettings()
						.fallbackAction() == 0,
					u"LLM error: "_q + error,
					true,
					latency);
			}
		});
}

void Gate::resolve(
		const Pending &pending,
		bool show,
		const QString &reason,
		bool isError,
		int ms) {
	if (isError) {
		++_counters.errors;
	}
	if (!pending.alive || !*pending.alive || !pending.session) {
		return;
	}
	const auto item = pending.session->data().message(pending.fullId);
	const auto chat = pending.session->data().peerLoaded(pending.historyPeerId)
		? pending.session->data().peerLoaded(pending.historyPeerId)->name()
		: u"chat"_q;
	if (!item) {
		log(chat, u"dropped"_q, u"message vanished"_q, ms);
		return;
	}
	const auto thread = item->notificationThread();
	if (show) {
		_shownTimes.push_back(crl::now());
		++_counters.shown;
		log(chat, u"shown"_q, reason, ms);
		playDeferredSound(thread, item);
		reentryShow(pending, item);
	} else {
		++_counters.suppressed;
		log(chat, u"suppressed"_q, reason, ms);
	}
}

void Gate::reentryShow(
		const Pending &pending,
		not_null<HistoryItem*> item) {
	auto reactionFrom = pending.reactionFromId
		? pending.session->data().peerLoaded(pending.reactionFromId)
		: nullptr;
	auto fields = Window::Notifications::Manager::NotificationFields{
		.item = item,
		.forwardedCount = pending.forwardedCount,
		.reactionFrom = reactionFrom,
		.reactionId = pending.reactionId,
		.pollVoteOption = pending.pollVoteOption,
		.soundId = pending.soundId,
	};
	_reentry = true;
	const auto guard = gsl::finally([&] {
		_reentry = false;
	});
	Core::App().notifications().manager().showNotification(
		std::move(fields));
}

void Gate::playDeferredSound(
		not_null<Data::Thread*> thread,
		not_null<HistoryItem*> item) {
	if (!_soundPlayer) {
		return;
	}
	const auto key = ThreadKey{
		.session = &thread->session(),
		.peerId = thread->peer()->id,
		.topicRootId = thread->topicRootId(),
		.monoforumPeerId = thread->monoforumPeerId(),
	};
	const auto now = crl::now();
	const auto it = _lastSoundAt.find(key);
	if (it != _lastSoundAt.end() && now - it->second < kSoundThrottleMs) {
		return;
	}
	if (_lastSoundAt.size() > 100) {
		_lastSoundAt.clear();
	}
	_lastSoundAt[key] = now;
	_soundPlayer(thread, item);
}

void Gate::SetSoundPlayer(SoundPlayer player) {
	_soundPlayer = std::move(player);
	if (!_soundPlayer) {
		CancelAll();
	}
}

void Gate::CancelHistory(not_null<History*> history) {
	const auto session = &history->session();
	const auto peerId = history->peer->id;
	_pending.erase(
		std::remove_if(
			_pending.begin(),
			_pending.end(),
			[&](const Pending &pending) {
				return pending.session == session
					&& pending.fullId.peer == peerId;
			}),
		_pending.end());
}

void Gate::CancelTopic(not_null<Data::ForumTopic*> topic) {
	const auto session = &topic->session();
	const auto peerId = topic->peer()->id;
	const auto topicRootId = topic->topicRootId();
	_pending.erase(
		std::remove_if(
			_pending.begin(),
			_pending.end(),
			[&](const Pending &pending) {
				return pending.session == session
					&& pending.fullId.peer == peerId
					&& pending.topicRootId == topicRootId;
			}),
		_pending.end());
}

void Gate::CancelSublist(not_null<Data::SavedSublist*> sublist) {
	const auto session = &sublist->session();
	const auto sublistPeerId = sublist->sublistPeer()->id;
	_pending.erase(
		std::remove_if(
			_pending.begin(),
			_pending.end(),
			[&](const Pending &pending) {
				return pending.session == session
					&& pending.monoforumPeerId == sublistPeerId;
			}),
		_pending.end());
}

void Gate::CancelSession(not_null<Main::Session*> session) {
	_pending.erase(
		std::remove_if(
			_pending.begin(),
			_pending.end(),
			[&](const Pending &pending) {
				return pending.session == session;
			}),
		_pending.end());
}

void Gate::CancelItem(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto fullId = item->fullId();
	_pending.erase(
		std::remove_if(
			_pending.begin(),
			_pending.end(),
			[&](const Pending &pending) {
				return pending.session == session
					&& pending.fullId == fullId;
			}),
		_pending.end());
}

void Gate::CancelAll() {
	++_generation;
	_pending.clear();
	_client->CancelAll();
}

void Gate::log(
		const QString &chat,
		const QString &verdict,
		const QString &reason,
		int ms) {
	_decisions.push_back({
		.ts = crl::now(),
		.chat = chat,
		.verdict = verdict,
		.reason = reason,
		.ms = ms,
	});
	while (int(_decisions.size()) > kLogLimit) {
		_decisions.pop_front();
	}
}

void Gate::resetStats() {
	_decisions.clear();
	_counters = {};
	_shownTimes.clear();
}

void Gate::TestConnection(Fn<void(QString)> done) {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	if (ai.endpoint().isEmpty() || ai.model().isEmpty()) {
		done(u"Endpoint and model must be set first."_q);
		return;
	}
	auto body = QJsonObject{
		{"model", ai.model()},
		{"temperature", 0},
		{"max_tokens", 5},
		{"messages", QJsonArray{
			QJsonObject{
				{"role", "user"},
				{"content", "Reply with the single word: OK"},
			},
		}},
	};
	const auto started = crl::now();
	_client->Send(
		{
			.endpoint = ai.endpoint(),
			.apiKey = ai.apiKey(),
			.body = QJsonDocument(body).toJson(QJsonDocument::Compact),
			.timeoutMs = ai.timeoutMs(),
		},
		[=](const QByteArray &raw) {
			const auto content = ExtractContent(raw);
			if (content.isEmpty()) {
				done(u"Connected, but unexpected response shape."_q);
				return;
			}
			done(u"OK ("_q
				+ QString::number(int(crl::now() - started))
				+ u" ms): "_q
				+ content.left(60));
		},
		[=](QString error) {
			done(u"Error: "_q + error);
		});
}

void Gate::TestVerdict(Fn<void(QString)> done) {
	const auto &ai = AyuSettings::getInstance().aiNotifySettings();
	if (ai.endpoint().isEmpty() || ai.model().isEmpty()) {
		done(u"Endpoint and model must be set first."_q);
		return;
	}
	auto sample = Payload{
		.id = 1,
		.chatType = u"group"_q,
		.chat = u"Example Chat"_q,
		.sender = u"Alex"_q,
		.text = u"Hey, are you coming to the meeting in 10 minutes?"_q,
		.mediaKind = u"text"_q,
	};
	const auto started = crl::now();
	const auto body = ComposeSingleRequest(sample, ai.systemPrompt());
	_client->Send(
		{
			.endpoint = ai.endpoint(),
			.apiKey = ai.apiKey(),
			.body = body,
			.timeoutMs = ai.timeoutMs(),
		},
		[=](const QByteArray &raw) {
			const auto content = ExtractContent(raw);
			done(u"("_q
				+ QString::number(int(crl::now() - started))
				+ u" ms) "_q
				+ (content.isEmpty()
					? u"unexpected response: "_q
					: u"verdict: "_q)
				+ content.left(300));
		},
		[=](QString error) {
			done(u"Error: "_q + error);
		});
}

} // namespace Ayu::AiNotify
