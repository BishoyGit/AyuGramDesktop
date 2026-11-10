// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ai_notify/ai_notify_payload.h"

#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_poll.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "history/history_item.h"
#include "ui/image/image_location.h"

#include <QtCore/QBuffer>
#include <QtGui/QImage>

namespace Ayu::AiNotify {
namespace {

[[nodiscard]] QString Cap(const QString &text, int cap) {
	if (cap <= 0 || text.size() <= cap) {
		return text;
	}
	return text.left(cap) + u"…"_q;
}

[[nodiscard]] QString Anonymize(not_null<PeerData*> peer) {
	const auto type = peer->isUser()
		? u"user"_q
		: peer->isChannel()
		? u"channel"_q
		: u"group"_q;
	return type + u" #"_q + QString::number(peer->id.value & 0xFFFFFFFFLL, 16);
}

[[nodiscard]] QString ChatTypeOf(not_null<PeerData*> peer) {
	if (peer->isUser()) {
		return peer->isBot() ? u"bot"_q : u"private"_q;
	} else if (peer->isChannel()) {
		return peer->isMegagroup() ? u"group"_q : u"channel"_q;
	}
	return u"group"_q;
}

[[nodiscard]] QByteArray DownscaleEncode(
		const QImage &source,
		int maxSide) {
	if (source.isNull()) {
		return {};
	}
	auto image = source;
	if (image.width() > maxSide || image.height() > maxSide) {
		image = image.scaled(
			maxSide,
			maxSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	auto buffer = QBuffer();
	buffer.open(QIODevice::WriteOnly);
	if (!image.save(&buffer, "JPEG", 80)) {
		return {};
	}
	return buffer.data();
}

// Pull the best locally available bytes for a photo. If nothing is cached
// yet we kick off a small-size download (a few dozen KB) — it may arrive
// while the notification waits for the batch window / LLM round-trip.
[[nodiscard]] QImage LoadPhotoImage(
		not_null<PhotoData*> photo,
		not_null<HistoryItem*> item,
		bool fetchSmall) {
	const auto view = photo->activeMediaView()
		? photo->activeMediaView()
		: photo->createMediaView();
	const auto origin = Data::FileOrigin(item->fullId());

	for (const auto size : {
		Data::PhotoSize::Large,
		Data::PhotoSize::Small,
		Data::PhotoSize::Thumbnail,
	}) {
		auto bytes = view->imageBytes(size);
		if (!bytes.isEmpty()) {
			auto image = QImage::fromData(bytes);
			if (!image.isNull()) {
				return image;
			}
		}
	}
	if (const auto image = view->image(Data::PhotoSize::Large)) {
		auto original = image->original();
		if (!original.isNull()) {
			return original;
		}
	}
	if (const auto inline_ = view->thumbnailInline()) {
		auto image = inline_->original();
		if (!image.isNull()) {
			return image;
		}
	}
	if (fetchSmall) {
		photo->load(
			Data::PhotoSize::Small,
			origin,
			LoadFromCloudOrLocal,
			false);
	}
	return {};
}

void CollectPhotoImages(
		not_null<HistoryItem*> item,
		std::vector<QByteArray> &to,
		int maxImages,
		int imageMaxSide,
		bool fetchSmall) {
	const auto media = item->media();
	if (!media || !media->photo()) {
		return;
	}
	const auto image = LoadPhotoImage(media->photo(), item, fetchSmall);
	if (image.isNull() || int(to.size()) >= maxImages) {
		return;
	}
	auto encoded = DownscaleEncode(image, imageMaxSide);
	if (!encoded.isEmpty()) {
		to.push_back(std::move(encoded));
	}
}

[[nodiscard]] QString DescribeDocument(not_null<DocumentData*> document) {
	if (document->isVoiceMessage()) {
		return u"voice message"_q;
	} else if (document->isVideoMessage()) {
		return u"video message"_q;
	} else if (document->isVideoFile()) {
		return u"video"_q;
	} else if (document->isAnimation()) {
		return u"gif"_q;
	} else if (document->sticker()) {
		const auto sticker = document->sticker();
		return u"sticker"_q + (sticker && !sticker->alt.isEmpty()
			? u" "_q + sticker->alt
			: QString());
	} else if (document->isAudioFile()) {
		return u"music"_q;
	}
	return u"file"_q;
}

} // namespace

Payload BuildPayload(
		not_null<HistoryItem*> item,
		bool anonymizeNames,
		bool sendText,
		int textCap,
		bool sendImages,
		int maxImages,
		int imageMaxSide,
		bool fetchSmallImages) {
	auto result = Payload();

	const auto history = item->history();
	const auto peer = history->peer;
	result.chatType = ChatTypeOf(peer);
	result.chat = anonymizeNames ? Anonymize(peer) : peer->name();
	if (const auto author = item->author()) {
		result.sender = anonymizeNames ? Anonymize(author) : author->name();
	}
	if (const auto topic = item->topic()) {
		result.topic = topic->title();
	}
	result.isMention = item->mentionsMe();

	if (sendText) {
		result.text = Cap(item->notificationText().text, textCap);
	}

	if (const auto replyTo = item->replyTo(); replyTo.messageId) {
		if (const auto replied = history->owner().message(replyTo.messageId)) {
			if (const auto author = replied->author()) {
				result.replyText = (anonymizeNames
					? Anonymize(author)
					: author->name()) + u": "_q;
			}
			result.replyText += Cap(
				replied->notificationText().text,
				300);
		}
	}

	const auto media = item->media();
	if (media && media->webpage()) {
		const auto page = media->webpage();
		auto parts = QStringList();
		if (!page->siteName.isEmpty()) {
			parts.push_back(page->siteName);
		}
		if (!page->title.isEmpty()) {
			parts.push_back(page->title);
		}
		if (!page->description.text.isEmpty()) {
			parts.push_back(page->description.text);
		}
		if (!parts.isEmpty()) {
			result.linkPreview = Cap(parts.join(u" — "_q), 300);
		}
	}

	if (media && media->photo()) {
		result.mediaKind = item->groupId() ? u"album"_q : u"photo"_q;
		if (sendImages) {
			// For albums collect every photo of the group, not just the
			// item the notification is attributed to.
			auto covered = 0;
			if (const auto group = history->owner().groups().find(item)) {
				for (const auto &groupItem : group->items) {
					if (int(result.imagesJpeg.size()) >= maxImages) {
						break;
					}
					const auto groupPhoto
						= groupItem->media()
							? groupItem->media()->photo()
							: nullptr;
					if (!groupPhoto) {
						continue;
					}
					++covered;
					const auto image = LoadPhotoImage(
						groupPhoto,
						groupItem,
						fetchSmallImages);
					if (image.isNull()) {
						continue;
					}
					auto encoded = DownscaleEncode(image, imageMaxSide);
					if (!encoded.isEmpty()) {
						result.imagesJpeg.push_back(std::move(encoded));
					}
				}
			} else {
				CollectPhotoImages(
					item,
					result.imagesJpeg,
					maxImages,
					imageMaxSide,
					fetchSmallImages);
			}
			if (result.imagesJpeg.empty()) {
				result.mediaNote = u"(image not loaded yet)"_q;
			}
		} else {
			result.mediaNote = u"(image withheld by privacy setting)"_q;
		}
	} else if (media && media->document()) {
		const auto document = media->document();
		result.mediaKind = DescribeDocument(document);
		const auto name = document->filename();
		if (!name.isEmpty() && result.mediaKind != u"sticker"_q) {
			result.mediaNote = name;
		}
		// Videos and video messages are deliberately NOT downloaded or
		// frame-grabbed: they are judged by their metadata only.
	} else if (media && media->poll()) {
		result.mediaKind = u"poll"_q;
		result.mediaNote = media->poll()->question.text;
	} else if (media && media->location()) {
		result.mediaKind = u"location"_q;
	} else if (media && media->sharedContact()) {
		result.mediaKind = u"contact"_q;
	}

	return result;
}

} // namespace Ayu::AiNotify
