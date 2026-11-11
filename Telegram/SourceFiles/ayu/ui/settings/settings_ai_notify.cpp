// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_ai_notify.h"

#include "ayu/ai_notify/ai_notify_client.h"
#include "ayu/ai_notify/ai_notify_filter.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_main.h"
#include "boxes/filters/edit_filter_chats_list.h"
#include "boxes/peer_list_box.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"
#include "ui/layers/generic_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <QtCore/QDateTime>
#include <QtCore/QTime>

namespace Settings {

using namespace Builder;
using namespace AyuBuilder;

namespace {

constexpr auto kChatLimit = 100;

[[nodiscard]] AiNotifySettings &Ai() {
	return AyuSettings::getInstance().aiNotifySettings();
}

[[nodiscard]] QString TimeText(int minutes) {
	const auto t = QTime(0, 0).addSecs(minutes * 60);
	return t.toString(u"HH:mm"_q);
}

void ShowResult(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	controller->show(Ui::MakeInformBox(text));
}

void EditMultilineBox(
		not_null<Window::SessionController*> controller,
		rpl::producer<QString> title,
		const QString &current,
		Fn<void(const QString &)> save) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(std::move(title));
		const auto field = box->addRow(
			object_ptr<Ui::InputField>(
				box->verticalLayout(),
				st::windowFilterNameInput,
				Ui::InputField::Mode::MultiLine,
				rpl::single(QString())),
			style::margins());
		field->setText(current);
		field->setMaxLength(100000);
		box->addButton(tr::lng_settings_save(), [=] {
			save(field->getLastText());
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void EditProviderBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"AI provider (OpenAI-compatible)"_q));
		const auto endpoint = box->addRow(
			object_ptr<Ui::InputField>(
				box->verticalLayout(),
				st::windowFilterNameInput,
				rpl::single(u"Endpoint URL (OpenAI, OpenRouter, Groq, Ollama...)"_q)),
			style::margins());
		const auto apiKey = box->addRow(
			object_ptr<Ui::InputField>(
				box->verticalLayout(),
				st::windowFilterNameInput,
				rpl::single(u"API key (empty for local servers)"_q)),
			style::margins());
		const auto model = box->addRow(
			object_ptr<Ui::InputField>(
				box->verticalLayout(),
				st::windowFilterNameInput,
				rpl::single(u"Model (vision-capable, e.g. gpt-4o-mini)"_q)),
			style::margins());
		endpoint->setText(Ai().endpoint());
		apiKey->setText(Ai().apiKey());
		model->setText(Ai().model());
		box->addButton(tr::lng_settings_save(), [=] {
			Ai().setEndpoint(endpoint->getLastText().trimmed());
			Ai().setApiKey(apiKey->getLastText());
			Ai().setModel(model->getLastText().trimmed());
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void PickChats(
		not_null<Window::SessionController*> controller,
		const QString &title,
		bool vip) {
	const auto session = &controller->session();
	const auto &ids = vip ? Ai().vipIds() : Ai().blockedIds();
	auto initial = base::flat_set<not_null<History*>>();
	for (const auto id : ids) {
		if (const auto peer = session->data().peerLoaded(PeerId(id))) {
			initial.insert(session->data().history(peer));
		}
	}
	const auto options = Data::ChatFilter::Flag::Contacts
		| Data::ChatFilter::Flag::NonContacts
		| Data::ChatFilter::Flag::Groups
		| Data::ChatFilter::Flag::Channels
		| Data::ChatFilter::Flag::Bots;
	auto chatsController = std::make_unique<EditFilterChatsListController>(
		session,
		rpl::single(title),
		options,
		options,
		initial,
		kChatLimit,
		[=] {
			controller->showToast(
				Ui::Text::Bold(u"Too many chats selected."_q));
		});
	auto initBox = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), crl::guard(box, [=] {
			const auto peers = box->collectSelectedRows();
			for (const auto id : ids) {
				if (vip) {
					Ai().removeVipId(id);
				} else {
					Ai().removeBlockedId(id);
				}
			}
			for (const auto &peer : peers) {
				if (vip) {
					Ai().addVipId(peer->id.value);
				} else {
					Ai().addBlockedId(peer->id.value);
				}
			}
			box->closeBox();
		}));
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	};
	controller->window().show(
		Box<PeerListBox>(std::move(chatsController), std::move(initBox)));
}

void ShowDecisionsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Recent AI decisions"_q));
		box->setWidth(st::boxWideWidth);
		const auto decisions = Ayu::AiNotify::Gate::Instance()
			.recentDecisions();
		const auto counters = Ayu::AiNotify::Gate::Instance().counters();
		const auto summary = u"Shown: %1   Suppressed: %2   Errors: %3"_q
			.arg(counters.shown)
			.arg(counters.suppressed)
			.arg(counters.errors);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box->verticalLayout(),
				summary,
				st::boxLabel),
			style::margins());
		if (decisions.empty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box->verticalLayout(),
					u"No decisions yet — they appear here as notifications arrive."_q,
					st::boxLabel),
				style::margins());
		}
		auto shown = 0;
		for (auto it = decisions.rbegin(); it != decisions.rend(); ++it) {
			if (++shown > 30) {
				break;
			}
			const auto when = QDateTime::fromMSecsSinceEpoch(it->ts)
				.time()
				.toString(u"HH:mm:ss"_q);
			const auto line = u"%1  %2  %3 — %4"_q
				.arg(when, it->verdict, it->chat, it->reason);
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box->verticalLayout(),
					line,
					st::boxLabel),
				style::margins());
		}
		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	}));
}

void BuildGeneral(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSkip();
	ayu.addToggle({
		.id = u"ayu/aiNotify/enable"_q,
		.title = rpl::single(u"Enable AI notification filter"_q),
		.getter = [=] { return Ai().enabled(); },
		.setter = [=](bool v) { Ai().setEnabled(v); },
		.icon = { &st::menuIconNotifications },
	});
	ayu.addToggle({
		.id = u"ayu/aiNotify/onlyUnfocused"_q,
		.title = rpl::single(u"Only filter when window is unfocused"_q),
		.getter = [=] { return Ai().onlyWhenUnfocused(); },
		.setter = [=](bool v) { Ai().setOnlyWhenUnfocused(v); },
		.icon = { &st::menuIconSoundOn },
	});
	builder.addDividerText(rpl::single(
		u"Every desktop notification is judged before it is shown. "
		"VIP chats always pass, blocked chats never show, and the rest "
		"goes to the AI when local rules can't decide."_q));
}

void BuildProvider(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();
	builder.addSubsectionTitle(rpl::single(u"Provider"_q));
	builder.addButton({
		.id = u"ayu/aiNotify/provider"_q,
		.title = rpl::single(u"Endpoint, API key & model..."_q),
		.icon = { &st::menuIconSettings },
		.onClick = [=] {
			EditProviderBox(controller);
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/prompt"_q,
		.title = rpl::single(u"System prompt..."_q),
		.icon = { &st::menuIconSettings },
		.onClick = [=] {
			EditMultilineBox(
				controller,
				rpl::single(u"System prompt (empty = default)"_q),
				Ai().systemPrompt(),
				[=](const QString &v) { Ai().setSystemPrompt(v); });
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/test"_q,
		.title = rpl::single(u"Test connection"_q),
		.icon = { &st::menuIconSoundOn },
		.onClick = [=] {
			Ayu::AiNotify::Gate::Instance().TestConnection(
				[=](const QString &result) {
					ShowResult(controller, result);
				});
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/testVerdict"_q,
		.title = rpl::single(u"Test AI verdict on a sample message"_q),
		.icon = { &st::menuIconSoundOn },
		.onClick = [=] {
			Ayu::AiNotify::Gate::Instance().TestVerdict(
				[=](const QString &result) {
					ShowResult(controller, result);
				});
		},
	});
}

void BuildBehavior(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSubsectionTitle(rpl::single(u"Behavior"_q));
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/fallback"_q,
		.title = rpl::single(u"When the AI fails or times out"_q),
		.boxTitle = rpl::single(u"Apply fallback action"_q),
		.initialSelection = Ai().fallbackAction(),
		.options = {
			u"Show the notifications"_q,
			u"Hide the notifications"_q },
		.setter = [=](int v) { Ai().setFallbackAction(v); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/maxPerHour"_q,
		.title = rpl::single(u"Max notifications per hour"_q),
		.boxTitle = rpl::single(u"Rate limit"_q),
		.initialSelection = [&] {
			const auto value = Ai().maxPerHour();
			return (value <= 10)
				? 0
				: (value <= 20)
				? 1
				: (value <= 40)
				? 2
				: (value <= 60)
				? 3
				: (value <= 120)
				? 4
				: 5;
		}(),
		.options = {
			u"10"_q, u"20"_q, u"40"_q, u"60"_q, u"120"_q, u"Unlimited"_q },
		.setter = [=](int v) {
			static constexpr int values[] = {
				10, 20, 40, 60, 120, 100000 };
			Ai().setMaxPerHour(values[v]);
		},
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/batch"_q,
		.title = rpl::single(u"Batch messages into one AI request"_q),
		.boxTitle = rpl::single(u"Batching window"_q),
		.initialSelection = [&] {
			const auto value = Ai().batchWindowMs();
			return (value <= 0)
				? 0
				: (value <= 1000)
				? 1
				: (value <= 1500)
				? 2
				: (value <= 3000)
				? 3
				: 4;
		}(),
		.options = {
			u"Off"_q,
			u"1 second"_q,
			u"1.5 seconds"_q,
			u"3 seconds"_q,
			u"5 seconds"_q },
		.setter = [=](int v) {
			Ai().setBatchWindowMs(v == 1
				? 1000
				: v == 2
				? 1500
				: v == 3
				? 3000
				: v == 4
				? 5000
				: 0);
		},
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/timeout"_q,
		.title = rpl::single(u"AI response timeout"_q),
		.boxTitle = rpl::single(u"Timeout"_q),
		.initialSelection = [&] {
			const auto value = Ai().timeoutMs();
			return (value <= 2000)
				? 0
				: (value <= 4000)
				? 1
				: (value <= 8000)
				? 2
				: 3;
		}(),
		.options = {
			u"2 seconds"_q, u"4 seconds"_q, u"8 seconds"_q, u"15 seconds"_q },
		.setter = [=](int v) {
			static constexpr int values[] = { 2000, 4000, 8000, 15000 };
			Ai().setTimeoutMs(values[v]);
		},
		.icon = { &st::menuIconSettings },
	});
}

void BuildQuietHours(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSubsectionTitle(rpl::single(u"Quiet hours"_q));
	ayu.addToggle({
		.id = u"ayu/aiNotify/quietEnabled"_q,
		.title = rpl::single(u"Enable quiet hours (VIP chats still pass)"_q),
		.getter = [=] { return Ai().quietHoursEnabled(); },
		.setter = [=](bool v) { Ai().setQuietHoursEnabled(v); },
		.icon = { &st::menuIconSoundOff },
	});
	const auto hourOptions = [] {
		auto result = std::vector<QString>();
		for (auto minute = 0; minute < 24 * 60; minute += 30) {
			result.push_back(TimeText(minute));
		}
		return result;
	}();
	const auto indexOfMinutes = [](int minutes) {
		return std::clamp(minutes, 0, 23 * 60 + 30) / 30;
	};
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/quietFrom"_q,
		.title = rpl::single(u"From"_q),
		.boxTitle = rpl::single(u"Quiet hours start"_q),
		.initialSelection = indexOfMinutes(Ai().quietFromMinutes()),
		.options = hourOptions,
		.setter = [=](int v) { Ai().setQuietFromMinutes(v * 30); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/quietTo"_q,
		.title = rpl::single(u"To"_q),
		.boxTitle = rpl::single(u"Quiet hours end"_q),
		.initialSelection = indexOfMinutes(Ai().quietToMinutes()),
		.options = hourOptions,
		.setter = [=](int v) { Ai().setQuietToMinutes(v * 30); },
		.icon = { &st::menuIconSettings },
	});
}

void BuildLists(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();
	builder.addSubsectionTitle(rpl::single(u"Lists & rules"_q));
	builder.addButton({
		.id = u"ayu/aiNotify/vip"_q,
		.title = rpl::single(u"VIP chats (always notify)..."_q),
		.icon = { &st::menuIconFave },
		.onClick = [=] {
			PickChats(controller, u"Always notify"_q, true);
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/blocked"_q,
		.title = rpl::single(u"Blocked chats (never notify)..."_q),
		.icon = { &st::menuIconReport },
		.onClick = [=] {
			PickChats(controller, u"Never notify"_q, false);
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/keywords"_q,
		.title = rpl::single(u"Keyword rules..."_q),
		.icon = { &st::menuIconManage },
		.onClick = [=] {
			EditMultilineBox(
				controller,
				rpl::single(
					u"Rules: +word (always show), -word (never show), "
					"word (never show), +re:regex..."_q),
				Ai().keywordRules(),
				[=](const QString &v) { Ai().setKeywordRules(v); });
		},
	});
	builder.addDividerText(rpl::single(
		u"One rule per line. Lines starting with '+' force the "
		"notification to show, '-' force it to hide, plain text hides. "
		"Prefix with 're:' to use a regular expression. Case-insensitive. "
		"Rules also match the media type (photo, video, voice...)."_q));
}

void BuildPrivacy(SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSubsectionTitle(rpl::single(u"Privacy (what is sent to the AI)"_q));
	ayu.addToggle({
		.id = u"ayu/aiNotify/sendText"_q,
		.title = rpl::single(u"Send message text"_q),
		.getter = [=] { return Ai().sendText(); },
		.setter = [=](bool v) { Ai().setSendText(v); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addToggle({
		.id = u"ayu/aiNotify/sendImages"_q,
		.title = rpl::single(u"Send images (photos of the post)"_q),
		.getter = [=] { return Ai().sendImages(); },
		.setter = [=](bool v) { Ai().setSendImages(v); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addToggle({
		.id = u"ayu/aiNotify/fetchSmallImages"_q,
		.title = rpl::single(
			u"Download small image preview if not cached (~50 KB)"_q),
		.getter = [=] { return Ai().fetchSmallImages(); },
		.setter = [=](bool v) { Ai().setFetchSmallImages(v); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addToggle({
		.id = u"ayu/aiNotify/anonymize"_q,
		.title = rpl::single(u"Hide chat & sender names from the AI"_q),
		.getter = [=] { return Ai().anonymizeNames(); },
		.setter = [=](bool v) { Ai().setAnonymizeNames(v); },
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/textCap"_q,
		.title = rpl::single(u"Max message text length"_q),
		.boxTitle = rpl::single(u"Text length cap"_q),
		.initialSelection = [&] {
			const auto value = Ai().textCap();
			return (value <= 200)
				? 0
				: (value <= 500)
				? 1
				: (value <= 1000)
				? 2
				: (value <= 4000)
				? 3
				: 4;
		}(),
		.options = {
			u"200 characters"_q,
			u"500 characters"_q,
			u"1 000 characters"_q,
			u"4 000 characters"_q,
			u"Whole post"_q },
		.setter = [=](int v) {
			static constexpr int values[] = { 200, 500, 1000, 4000, 100000 };
			Ai().setTextCap(values[v]);
		},
		.icon = { &st::menuIconSettings },
	});
	ayu.addChooseButton({
		.id = u"ayu/aiNotify/maxImages"_q,
		.title = rpl::single(u"Max images per notification"_q),
		.boxTitle = rpl::single(u"Max images"_q),
		.initialSelection = std::clamp(Ai().maxImages(), 1, 6) - 1,
		.options = { u"1"_q, u"2"_q, u"3"_q, u"4"_q, u"5"_q, u"6"_q },
		.setter = [=](int v) { Ai().setMaxImages(v + 1); },
		.icon = { &st::menuIconSettings },
	});
}

void BuildDiagnostics(SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addSubsectionTitle(rpl::single(u"Diagnostics"_q));
	builder.addButton({
		.id = u"ayu/aiNotify/log"_q,
		.title = rpl::single(u"Recent decisions..."_q),
		.icon = { &st::menuIconInfo },
		.onClick = [=] {
			ShowDecisionsBox(controller);
		},
	});
	builder.addButton({
		.id = u"ayu/aiNotify/reset"_q,
		.title = rpl::single(u"Reset statistics & log"_q),
		.icon = { &st::menuIconCustomize },
		.onClick = [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = rpl::single(u"Reset AI filter statistics and "
					"decision log?"_q),
				.confirmed = [=] {
					Ayu::AiNotify::Gate::Instance().resetStats();
				},
			}));
		},
	});
}

const auto kMeta = BuildHelper({
	.id = AyuAiNotify::Id(),
	.parentId = AyuMain::Id(),
	.title = QString::fromUtf8("AI Notification Filter"),
	.icon = &st::menuIconNotifications,
}, [](SectionBuilder &builder) {
	builder.addSkip();
	BuildGeneral(builder);
	BuildProvider(builder);
	BuildBehavior(builder);
	BuildQuietHours(builder);
	BuildLists(builder);
	BuildPrivacy(builder);
	BuildDiagnostics(builder);
});

} // namespace

rpl::producer<QString> AyuAiNotify::title() {
	return rpl::single(QString::fromUtf8("AI Notification Filter"));
}

AyuAiNotify::AyuAiNotify(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuAiNotify::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuAiNotifyId() {
	return AyuAiNotify::Id();
}

} // namespace Settings
