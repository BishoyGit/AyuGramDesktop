// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ai_notify/ai_notify_client.h"

#include <algorithm>
#include <gsl/gsl_util>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Ayu::AiNotify {

AiClient::AiClient(not_null<QObject*> parent) : QObject(parent) {
}

void AiClient::Send(const Request &request, Done done, Fail fail) {
	if (!_nam) {
		_nam = new QNetworkAccessManager(this);
	}

	auto url = QUrl(request.endpoint);
	if (!url.isValid() || url.scheme().isEmpty()) {
		fail(u"Invalid endpoint URL: "_q + request.endpoint);
		return;
	}

	auto req = QNetworkRequest(url);
	req.setHeader(
		QNetworkRequest::KnownHeaders::ContentTypeHeader,
		"application/json");
	if (!request.apiKey.isEmpty()) {
		req.setRawHeader("Authorization", "Bearer " + request.apiKey.toUtf8());
	}
	req.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setTransferTimeout(std::max(request.timeoutMs, 500));

	auto *reply = _nam->post(req, request.body);
	_inFlight.push_back(reply);

	connect(reply, &QNetworkReply::finished, this, [=] {
		const auto guard = gsl::finally([&] {
			reply->deleteLater();
		});
		_inFlight.erase(
			std::remove(_inFlight.begin(), _inFlight.end(), reply),
			_inFlight.end());

		if (reply->error() != QNetworkReply::NoError) {
			auto message = QString(reply->errorString());
			const auto body = reply->readAll();
			const auto parsed = QJsonDocument::fromJson(body);
			if (parsed.isObject()) {
				const auto error = parsed.object().value("error");
				if (error.isObject()) {
					const auto text = error.toObject().value("message");
					if (text.isString()) {
						message = text.toString();
					}
				}
			}
			fail(message);
			return;
		}
		done(reply->readAll());
	});
}

void AiClient::CancelAll() {
	for (const auto &reply : _inFlight) {
		if (reply) {
			reply->abort();
		}
	}
	_inFlight.clear();
}

QString DefaultSystemPrompt() {
	return u"You filter Telegram desktop notifications for the user. "
	"You receive one JSON array of pending notifications, possibly with "
	"attached images for photo messages. For EVERY item (match by its \"id\") "
	"decide whether it is worth interrupting the user right now.\n"
	"Show (true): direct messages to the user, questions, urgent or personal "
	"content, mentions of the user, replies in an ongoing conversation, "
	"important security or delivery codes.\n"
	"Hide (false): bot spam, promotional content, newsletters, channel posts "
	"of low personal relevance, routine automated alerts, repetitive group "
	"chatter, unrelated memes.\n"
	"Use attached images to judge importance when present. Videos are only "
	"described by metadata - judge those by text and context.\n"
	"Answer with STRICT JSON only, no markdown: "
	"{\"results\":[{\"id\":1,\"show\":true,\"reason\":\"max 8 words\"}]}"_q;
}

} // namespace Ayu::AiNotify
