// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>

class QNetworkAccessManager;
class QNetworkReply;

namespace Ayu::AiNotify {

// Minimal OpenAI-compatible `/chat/completions` transport.
// Works with OpenAI, OpenRouter, Groq, Ollama, LM Studio, ...
class AiClient final : public QObject {
public:
	struct Request {
		QString endpoint;
		QString apiKey;
		QByteArray body;
		int timeoutMs = 4000;
	};

	explicit AiClient(not_null<QObject*> parent);
	using Done = Fn<void(const QByteArray &)>;
	using Fail = Fn<void(QString)>;

	void Send(const Request &request, Done done, Fail fail);
	void CancelAll();

	[[nodiscard]] bool hasInFlight() const {
		return !_inFlight.empty();
	}
	[[nodiscard]] int inFlightCount() const {
		return int(_inFlight.size());
	}

private:
	QNetworkAccessManager *_nam = nullptr;
	std::vector<QPointer<QNetworkReply>> _inFlight;
};

[[nodiscard]] QString DefaultSystemPrompt();

} // namespace Ayu::AiNotify
