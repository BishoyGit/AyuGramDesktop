// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

class HistoryItem;
class PhotoData;

namespace Ayu::AiNotify {

// A full description of one pending notification, ready to be judged.
struct Payload {
	int id = 0;
	QString chatType; // "private" / "bot" / "group" / "channel"
	QString chat;
	QString sender;
	QString topic;
	QString text;
	QString mediaKind; // "text" / "photo" / "album" / "video" / "gif" / ...
	QString mediaNote; // filename, album size, sticker emoji, ...
	QString replyText;
	QString linkPreview;
	bool isMention = false;
	bool imagesPending = false;
	std::vector<QByteArray> imagesJpeg;
};

// Collects everything known about the message locally: full text, media
// classification, images (photos only — videos are ignored), reply context
// and link preview. Images are requested at the largest available size.
[[nodiscard]] Payload BuildPayload(
	not_null<HistoryItem*> item,
	bool anonymizeNames,
	bool sendText,
	int textCap,
	bool sendImages,
	int maxImages);

void RequestLargeImages(not_null<HistoryItem*> item, int maxImages);

} // namespace Ayu::AiNotify
