// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026

#include "ayu/features/filters/filters_controller.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "ayu/features/filters/filters_utils.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "unicode/regex.h"

#include <memory>
#include <unordered_set>

namespace FiltersController {

std::unordered_set<long long> showingFilteredMessages;
std::unordered_set<long long> mediaOnlyPeers;
std::unordered_set<long long> hideServicePeers;

bool filterBlocked(const not_null<HistoryItem*> item) {
	if (item->from() != item->history()->peer) {
		if (isBlocked(item)) {
			return true;
		}
	}
	if (const auto bot = item->viaBot()) {
		if (isBlocked(bot)) {
			return true;
		}
	}
	return false;
}

std::optional<bool> isFiltered(
		const QString &str,
		long long dialogId,
		const std::shared_ptr<const FiltersCacheController::Cache> &cache) {
	if (str.isEmpty()) {
		return std::nullopt;
	}

	const auto icuStr = icu::UnicodeString(reinterpret_cast<const UChar*>(str.constData()), str.length());

	const auto matches = [&](const ReversiblePattern &pattern)
	{
		UErrorCode status = U_ZERO_ERROR;

		const auto matcher = std::unique_ptr<icu::RegexMatcher>(pattern.pattern->matcher(icuStr, status));
		if (U_FAILURE(status) || !matcher) {
			LOG(("FILTER FAILED: %1").arg(u_errorName(status)));
			return false;
		}

		const auto match = matcher->find();
		const auto reversed = pattern.reversed;

		if ((!reversed && match) || (reversed && !match)) {
			return true;
		}
		return false;
	};

	if (const auto i = cache->patternsByDialogId.find(dialogId); i != cache->patternsByDialogId.end()) {
		for (const auto &pattern : i->second) {
			if (matches(pattern)) {
				return true;
			}
		}
	}

	const auto exclusions = cache->exclusionsByDialogId.find(dialogId);
	if (!cache->sharedPatterns.empty()) {
		for (const auto &pattern : cache->sharedPatterns) {
			if (exclusions != cache->exclusionsByDialogId.end() && exclusions->second.contains(pattern)) {
				continue;
			}
			if (matches(pattern.pattern)) {
				return true;
			}
		}
	}
	return false;
}

bool isEnabled(not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled() && (settings.filtersEnabledInChats() || peer->isBroadcast());
}

bool isBlocked(const not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();

	auto shadowBanMatched = false;
	const auto blocked = [&]() -> bool
	{
		const auto isShadowBanned = [&](PeerData *peer) {
			return peer
				&& (peer->isUser() || peer->isBroadcast())
				&& settings.isShadowBanned(getDialogIdFromPeer(peer));
		};

		if (isShadowBanned(item->from())
			&& item->from()->id != item->history()->peer->id) {
			shadowBanMatched = true;
			return true;
		}

		if (item->from()->isUser()
			&& item->from()->asUser()->isBlocked()) {
			// don't hide messages if it's a dialog with blocked user
			return item->from()->asUser()->id != item->history()->peer->id;
		}

		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (const auto originalSender = forwarded->originalSender) {
				const auto originalShadowBanned = isShadowBanned(originalSender);
				if (originalShadowBanned
					|| (originalSender->isUser()
						&& originalSender->asUser()->isBlocked())) {
					shadowBanMatched = originalShadowBanned;
					return true;
				}
			}
		}
		return false;
	}();

	return settings.filtersEnabled()
		&& (shadowBanMatched || settings.hideFromBlocked())
		&& blocked;
}

bool isBlocked(const not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled() &&
	(
		(peer->isUser() && peer->asUser()->isBlocked() && settings.hideFromBlocked()) ||
		((peer->isUser() || peer->isBroadcast()) && settings.isShadowBanned(getDialogIdFromPeer(peer)))
	);
}

bool filtered(const not_null<HistoryItem*> item) {
	if (showingFilteredMessages.contains(item->history()->peer->id.value)) {
		return false;
	}

	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return false;
	}

	if (item->out()) {
		return false;
	}

	if (filterBlocked(item)) {
		FiltersCacheController::putHiddenBlockedMessage(item);
		return true;
	}

	if (!isEnabled(item->history()->peer)) return false;

	const auto cached = FiltersCacheController::isFiltered(item);
	if (cached.has_value()) {
		return cached.value();
	}
	const auto group = item->history()->owner().groups().find(item);
	const auto cache = FiltersCacheController::snapshot();
	const auto res = isFiltered(
		FilterUtils::extractAllText(item, group),
		getDialogIdFromPeer(item->history()->peer),
		cache);

	// sometimes item has empty text.
	// so we cache result only if
	// processed item is filterable
	if (res.has_value()) {
		FiltersCacheController::putFiltered(item, group, res.value(), cache);
		return res.value();
	}
	return false;
}

std::optional<bool> filteredMessagesShown(not_null<PeerData*> peer) {
	if (!showingFilteredMessages.contains(peer->id.value)
		&& !FiltersCacheController::hasFilteredMessages(peer)) {
		return std::nullopt;
	}
	return showingFilteredMessages.contains(peer->id.value);
}

void toggleFilteredMessagesShown(not_null<PeerData*> peer) {
	if (showingFilteredMessages.contains(peer->id.value)) {
		showingFilteredMessages.erase(peer->id.value);
	} else {
		showingFilteredMessages.insert(peer->id.value);
	}
	FiltersCacheController::fireUpdate();
}

void invalidate(not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return;
	}

	FiltersCacheController::invalidate(item);
}

bool mediaOnlyView(not_null<PeerData*> peer) {
	return mediaOnlyPeers.contains(peer->id.value);
}

void toggleMediaOnlyView(not_null<PeerData*> peer) {
	if (!mediaOnlyPeers.erase(peer->id.value)) {
		mediaOnlyPeers.insert(peer->id.value);
	}
	FiltersCacheController::fireUpdate();
}

bool hideServiceView(not_null<PeerData*> peer) {
	return hideServicePeers.contains(peer->id.value);
}

void toggleHideServiceView(not_null<PeerData*> peer) {
	if (!hideServicePeers.erase(peer->id.value)) {
		hideServicePeers.insert(peer->id.value);
	}
	FiltersCacheController::fireUpdate();
}

bool hiddenByChatView(not_null<HistoryItem*> item) {
	if (mediaOnlyPeers.empty() && hideServicePeers.empty()) {
		return false;
	}
	const auto id = item->history()->peer->id.value;
	const auto mediaOnly = mediaOnlyPeers.contains(id);
	const auto hideService = mediaOnly || hideServicePeers.contains(id);
	if (item->isService()) {
		return hideService;
	} else if (!mediaOnly) {
		return false;
	}
	// Media only: photos, videos, GIFs, files, music and voice notes.
	const auto media = item->media();
	if (!media || media->webpage()) {
		return true;
	} else if (const auto document = media->document()) {
		return document->sticker() != nullptr;
	}
	return !media->photo();
}

}
