// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/top_messages_view/tm_inner.h"

#include "apiwrap.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "api/api_attached_stickers.h"
#include "ayu/data/messages_storage.h"
#include "ayu/ui/top_messages_view/tm_section.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "base/platform/base_platform_info.h"
#include "base/qt/qt_key_modifiers.h"
#include "boxes/sticker_set_box.h"
#include "boxes/peers/edit_participant_box.h"
#include "chat_helpers/message_field.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "crl/crl_async.h"
#include "data/data_cloud_file.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_forum.h"
#include "data/data_groups.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_text.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_service_message.h"
#include "history/view/media/history_view_media.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "styles/style_chat.h"
#include "styles/style_menu_icons.h"
#include "ui/inactive_press.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "media/view/media_view_overlay_widget.h"

#include <QtGui/QClipboard>
#include <QtWidgets/QApplication>

namespace TopMessagesView {
namespace {

constexpr auto kScrollDateHideTimeout = 1000;



constexpr auto kClearUserpicsAfter = 50;

} // namespace

template<InnerWidget::EnumItemsDirection direction, typename Method>
void InnerWidget::enumerateItems(Method method) {
	constexpr auto TopToBottom = (direction == EnumItemsDirection::TopToBottom);

	// No displayed messages in this history.
	if (_items.empty()) {
		return;
	}
	if (_visibleBottom <= _itemsTop || _itemsTop + _itemsHeight <= _visibleTop) {
		return;
	}

	auto begin = std::rbegin(_items), end = std::rend(_items);
	auto from = TopToBottom
					? std::lower_bound(begin,
									   end,
									   _visibleTop,
									   [this](auto &elem, int top)
									   {
										   return this->itemTop(elem) + elem->height() <= top;
									   })
					: std::upper_bound(begin,
									   end,
									   _visibleBottom,
									   [this](int bottom, auto &elem)
									   {
										   return this->itemTop(elem) + elem->height() >= bottom;
									   });
	auto wasEnd = (from == end);
	if (wasEnd) {
		--from;
	}
	if (TopToBottom) {
		Assert(itemTop(from->get()) + from->get()->height() > _visibleTop);
	} else {
		Assert(itemTop(from->get()) < _visibleBottom);
	}

	while (true) {
		auto item = from->get();
		auto itemtop = itemTop(item);
		auto itembottom = itemtop + item->height();

		// Binary search should've skipped all the items that are above / below the visible area.
		if (TopToBottom) {
			Assert(itembottom > _visibleTop);
		} else {
			Assert(itemtop < _visibleBottom);
		}

		if (!method(item, itemtop, itembottom)) {
			return;
		}

		// Skip all the items that are below / above the visible area.
		if (TopToBottom) {
			if (itembottom >= _visibleBottom) {
				return;
			}
		} else {
			if (itemtop <= _visibleTop) {
				return;
			}
		}

		if (TopToBottom) {
			if (++from == end) {
				break;
			}
		} else {
			if (from == begin) {
				break;
			}
			--from;
		}
	}
}

template<typename Method>
void InnerWidget::enumerateUserpics(Method method) {
	// Find and remember the top of an attached messages pack
	// -1 means we didn't find an attached to next message yet.
	int lowestAttachedItemTop = -1;

	auto userpicCallback = [&](not_null<Element*> view, int itemtop, int itembottom)
	{
		// Skip all service messages.
		if (view->data()->isService()) {
			return true;
		}

		if (lowestAttachedItemTop < 0 && view->isAttachedToNext()) {
			lowestAttachedItemTop = itemtop + view->marginTop();
		}

		// Call method on a userpic for all messages that have it and for those who are not showing it
		// because of their attachment to the next message if they are bottom-most visible.
		if (view->displayFromPhoto() || (view->hasFromPhoto() && itembottom >= _visibleBottom)) {
			if (lowestAttachedItemTop < 0) {
				lowestAttachedItemTop = itemtop + view->marginTop();
			}
			// Attach userpic to the bottom of the visible area with the same margin as the last message.
			auto userpicMinBottomSkip = st::historyPaddingBottom + st::msgMargin.bottom();
			auto userpicBottom = qMin(itembottom - view->marginBottom(), _visibleBottom - userpicMinBottomSkip);

			// Do not let the userpic go above the attached messages pack top line.
			userpicBottom = qMax(userpicBottom, lowestAttachedItemTop + st::msgPhotoSize);

			// Call the template callback function that was passed
			// and return if it finished everything it needed.
			if (!method(view, userpicBottom - st::msgPhotoSize)) {
				return false;
			}
		}

		// Forget the found top of the pack, search for the next one from scratch.
		if (!view->isAttachedToNext()) {
			lowestAttachedItemTop = -1;
		}

		return true;
	};

	enumerateItems<EnumItemsDirection::TopToBottom>(userpicCallback);
}

template<typename Method>
void InnerWidget::enumerateDates(Method method) {
	// Find and remember the bottom of an single-day messages pack
	// -1 means we didn't find a same-day with previous message yet.
	auto lowestInOneDayItemBottom = -1;

	auto dateCallback = [&](not_null<Element*> view, int itemtop, int itembottom)
	{
		const auto item = view->data();
		if (lowestInOneDayItemBottom < 0 && view->isInOneDayWithPrevious()) {
			lowestInOneDayItemBottom = itembottom - view->marginBottom();
		}

		// Call method on a date for all messages that have it and for those who are not showing it
		// because they are in a one day together with the previous message if they are top-most visible.
		if (view->displayDate() || (!item->isEmpty() && itemtop <= _visibleTop)) {
			if (lowestInOneDayItemBottom < 0) {
				lowestInOneDayItemBottom = itembottom - view->marginBottom();
			}
			// Attach date to the top of the visible area with the same margin as it has in service message.
			auto dateTop = qMax(itemtop, _visibleTop) + st::msgServiceMargin.top();

			// Do not let the date go below the single-day messages pack bottom line.
			auto dateHeight = st::msgServicePadding.bottom() + st::msgServiceFont->height + st::msgServicePadding.top();
			dateTop = qMin(dateTop, lowestInOneDayItemBottom - dateHeight);

			// Call the template callback function that was passed
			// and return if it finished everything it needed.
			if (!method(view, itemtop, dateTop)) {
				return false;
			}
		}

		// Forget the found bottom of the pack, search for the next one from scratch.
		if (!view->isInOneDayWithPrevious()) {
			lowestInOneDayItemBottom = -1;
		}

		return true;
	};

	enumerateItems<EnumItemsDirection::BottomToTop>(dateCallback);
}

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	std::vector<FullMsgId> ids)
	: RpWidget(parent),
	  _controller(controller),
	  _peer(peer),
	  _ids(std::move(ids)),
	  _history(peer->owner().history(peer)),
	  _api(&_peer->session().mtp()),
	  _pathGradient(
		  HistoryView::MakePathShiftGradient(
			  controller->chatStyle(),
			  [=]
			  {
				  update();
			  })),
	  _scrollDateCheck([=]
	  {
		  scrollDateCheck();
	  }),
	  _emptyText(
		  st::historyAdminLogEmptyWidth
		  - st::historyAdminLogEmptyPadding.left()
		  - st::historyAdminLogEmptyPadding.left()) {
	Window::ChatThemeValueFromPeer(
		controller,
		peer
	) | rpl::on_next([=](std::shared_ptr<Ui::ChatTheme> &&theme)
							 {
								 _theme = std::move(theme);
								 controller->setChatStyleTheme(_theme);
							 },
							 lifetime());

	// These are real messages: drop our view if one gets deleted.
	session().data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		removeRealItem(item);
	}, lifetime());

	setMouseTracking(true);
	_scrollDateHideTimer.setCallback([=]
	{
		scrollDateHideByTimer();
	});
	session().data().viewRepaintRequest(
	) | rpl::on_next([=](Data::RequestViewRepaint data)
							 {
								 if (data.view->delegate() == this) {
									 repaintItem(data.view);
								 }
							 },
							 lifetime());
	session().data().viewResizeRequest(
	) | rpl::on_next([=](auto view)
							 {
								 if (view->delegate() == this) {
									 resizeItem(view);
								 }
							 },
							 lifetime());
	session().data().itemViewRefreshRequest(
	) | rpl::on_next([=](auto item)
							 {
								 if (const auto view = viewForItem(item)) {
									 refreshItem(view);
								 }
							 },
							 lifetime());
	session().data().viewLayoutChanged(
	) | rpl::on_next([=](auto view)
							 {
								 if (view->delegate() == this) {
									 if (view->isUnderCursor()) {
										 updateSelected();
									 }
								 }
							 },
							 lifetime());
	session().data().itemDataChanges(
	) | rpl::on_next([=](not_null<HistoryItem*> item)
							 {
								 if (const auto view = viewForItem(item)) {
									 view->itemDataChanged();
								 }
							 },
							 lifetime());
	session().data().itemVisibilityQueries(
	) | rpl::filter([=](
		const Data::Session::ItemVisibilityQuery &query)
		{
			return (_history == query.item->history())
				&& isVisible();
		}) | rpl::on_next([=](
								  const Data::Session::ItemVisibilityQuery &query)
								  {
									  if (const auto view = viewForItem(query.item)) {
										  auto top = itemTop(view);
										  if (top >= 0
											  && top + view->height() > _visibleTop
											  && top < _visibleBottom) {
											  *query.isVisible = true;
										  }
									  }
								  },
								  lifetime());

	updateEmptyText();
}

Main::Session &InnerWidget::session() const {
	return _controller->session();
}

rpl::producer<int> InnerWidget::scrollToSignal() const {
	return _scrollToSignal.events();
}

void InnerWidget::visibleTopBottomUpdated(
	int visibleTop,
	int visibleBottom) {
	auto scrolledUp = (visibleTop < _visibleTop);
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;

	// Unload userpics.
	if (_userpics.size() > kClearUserpicsAfter) {
		_userpicsCache = std::move(_userpics);
	}

	updateVisibleTopItem();
	checkPreloadMore();
	if (scrolledUp) {
		_scrollDateCheck.call();
	} else {
		scrollDateHideByTimer();
	}
	_controller->floatPlayerAreaUpdated();
	session().data().itemVisibilitiesUpdated();
}

void InnerWidget::updateVisibleTopItem() {
	if (_visibleBottom == height()) {
		_visibleTopItem = nullptr;
	} else {
		auto begin = std::rbegin(_items), end = std::rend(_items);
		auto from = std::lower_bound(begin,
									 end,
									 _visibleTop,
									 [this](auto &&elem, int top)
									 {
										 return this->itemTop(elem) + elem->height() <= top;
									 });
		if (from != end) {
			_visibleTopItem = *from;
			_visibleTopFromItem = _visibleTop - _visibleTopItem->y();
		} else {
			_visibleTopItem = nullptr;
			_visibleTopFromItem = _visibleTop;
		}
	}
}

bool InnerWidget::displayScrollDate() const {
	return (_visibleTop <= height() - 2 * (_visibleBottom - _visibleTop));
}

void InnerWidget::scrollDateCheck() {
	if (!_visibleTopItem) {
		_scrollDateLastItem = nullptr;
		_scrollDateLastItemTop = 0;
		scrollDateHide();
	} else if (_visibleTopItem != _scrollDateLastItem || _visibleTopFromItem != _scrollDateLastItemTop) {
		// Show scroll date only if it is not the initial onScroll() event (with empty _scrollDateLastItem).
		if (_scrollDateLastItem && !_scrollDateShown) {
			toggleScrollDateShown();
		}
		_scrollDateLastItem = _visibleTopItem;
		_scrollDateLastItemTop = _visibleTopFromItem;
		_scrollDateHideTimer.callOnce(kScrollDateHideTimeout);
	}
}

void InnerWidget::scrollDateHideByTimer() {
	_scrollDateHideTimer.cancel();
	scrollDateHide();
}

void InnerWidget::scrollDateHide() {
	if (_scrollDateShown) {
		toggleScrollDateShown();
	}
}

void InnerWidget::toggleScrollDateShown() {
	_scrollDateShown = !_scrollDateShown;
	auto from = _scrollDateShown ? 0. : 1.;
	auto to = _scrollDateShown ? 1. : 0.;
	_scrollDateOpacity.start([this]
							 {
								 repaintScrollDateCallback();
							 },
							 from,
							 to,
							 st::historyDateFadeDuration);
}

void InnerWidget::repaintScrollDateCallback() {
	auto updateTop = _visibleTop;
	auto updateHeight = st::msgServiceMargin.top() + st::msgServicePadding.top() + st::msgServiceFont->height
		+ st::msgServicePadding.bottom();
	update(0, updateTop, width(), updateHeight);
}

void InnerWidget::checkPreloadMore() {
	if (_visibleTop + PreloadHeightsCount * (_visibleBottom - _visibleTop) > height()) {
		preloadMore(Direction::Down);
	}
	if (_visibleTop < PreloadHeightsCount * (_visibleBottom - _visibleTop)) {
		preloadMore(Direction::Up);
	}
}

void InnerWidget::updateEmptyText() {
	// auto text = !_searchQuery.isEmpty()
	// 	? tr::lng_admin_log_no_results_title(tr::now)
	// 	: tr::lng_search_messages_none(tr::now);
	_emptyText.setMarkedText(st::defaultTextStyle, Ui::Text::Semibold(tr::lng_search_messages_none(tr::now)));
}

QString InnerWidget::tooltipText() const {
	if (_mouseCursorState == CursorState::Date
		&& _mouseAction == MouseAction::None) {
		if (const auto view = Element::Hovered()) {
			auto dateText = HistoryView::DateTooltipText(view);

			const auto sentIt = _itemDates.find(view->data());
			if (sentIt != end(_itemDates)) {
				dateText += '\n' + tr::lng_sent_date(
					tr::now,
					lt_date,
					QLocale().toString(
						base::unixtime::parse(sentIt->second),
						QLocale::LongFormat));
			}
			return dateText;
		}
	} else if (_mouseCursorState == CursorState::Forwarded
		&& _mouseAction == MouseAction::None) {
		if (const auto view = Element::Hovered()) {
			if (const auto forwarded = view->data()->Get<HistoryMessageForwarded>()) {
				return forwarded->text.toString();
			}
		}
	} else if (const auto lnk = ClickHandler::getActive()) {
		return lnk->tooltip();
	}
	return QString();
}

QPoint InnerWidget::tooltipPos() const {
	return _mousePosition;
}

bool InnerWidget::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

HistoryView::Context InnerWidget::elementContext() {
	return HistoryView::Context::AdminLog;
}

bool InnerWidget::elementUnderCursor(
	not_null<const HistoryView::Element*> view) {
	return (Element::Hovered() == view);
}

HistoryView::SelectionModeResult InnerWidget::elementInSelectionMode(const HistoryView::Element *) {
	return {};
}

bool InnerWidget::elementIntersectsRange(
	not_null<const Element*> view,
	int from,
	int till) {
	Expects(view->delegate() == this);

	const auto top = itemTop(view);
	const auto bottom = top + view->height();
	return (top < till && bottom > from);
}

void InnerWidget::elementStartStickerLoop(not_null<const Element*> view) {
}

void InnerWidget::elementShowPollResults(
	not_null<PollData*> poll,
	FullMsgId context) {
}

void InnerWidget::elementShowAddPollOption(
	not_null<Element*> view,
	not_null<PollData*> poll,
	FullMsgId context,
	QRect optionRect) {
}

void InnerWidget::elementSubmitAddPollOption(FullMsgId context) {
}

void InnerWidget::elementOpenPhoto(
	not_null<PhotoData*> photo,
	FullMsgId context) {
	Media::View::SetNextOpenCustomOrder(mediaOrder());
	_controller->openPhoto(photo, {context});
}

void InnerWidget::elementOpenDocument(
	not_null<DocumentData*> document,
	FullMsgId context,
	bool showInMediaView) {
	Media::View::SetNextOpenCustomOrder(mediaOrder());
	_controller->openDocument(document, showInMediaView, {context});
}

bool InnerWidget::elementScrollToLocalY(
		not_null<const Element*> view,
		int localTop) {
	const auto currentScrollHeight = _visibleBottom - _visibleTop;
	const auto wanted = std::max(
		std::min(itemTop(view) + localTop, height() - currentScrollHeight),
		0);
	if (wanted != _visibleTop) {
		_scrollToSignal.fire_copy(wanted);
	}
	return true;
}

void InnerWidget::elementCancelUpload(const FullMsgId &context) {
	if (const auto item = session().data().message(context)) {
		_controller->cancelUploadLayer(item);
	}
}

void InnerWidget::elementShowTooltip(
	const TextWithEntities &text,
	Fn<void()> hiddenCallback) {
}

void InnerWidget::elementShowHiddenSenderTooltip(
		FullMsgId itemId,
		const TextWithEntities &text) {
	_controller->showToast(TextWithEntities(text));
}

bool InnerWidget::elementAnimationsPaused() {
	return _controller->isGifPausedAtLeastFor(Window::GifPauseReason::Any);
}

bool InnerWidget::elementHideReply(not_null<const Element*> view) {
	return true;
}

bool InnerWidget::elementShownUnread(not_null<const Element*> view) {
	return false;
}

void InnerWidget::elementSendBotCommand(
	const QString &command,
	const FullMsgId &context) {
}

void InnerWidget::elementSearchInList(
	const QString &query,
	const FullMsgId &context) {
}

void InnerWidget::elementHandleViaClick(not_null<UserData*> bot) {
}

HistoryView::ElementChatMode InnerWidget::elementChatMode() {
	return HistoryView::ElementChatMode::Wide;
}

not_null<Ui::PathShiftGradient*> InnerWidget::elementPathShiftGradient() {
	return _pathGradient.get();
}

void InnerWidget::elementReplyTo(const FullReplyTo &to) {
}

void InnerWidget::elementStartInteraction(not_null<const Element*> view) {
}

void InnerWidget::elementStartPremium(
	not_null<const HistoryView::Element*> view,
	HistoryView::Element *replacing) {
}

void InnerWidget::elementCancelPremium(not_null<const Element*> view) {
}

void InnerWidget::elementStartEffect(
	not_null<const Element*> view,
	Element *replacing) {
}

QString InnerWidget::elementAuthorRank(not_null<const Element*> view) {
	return {};
}

bool InnerWidget::elementHideTopicButton(not_null<const Element*> view) {
	return false;
}

void InnerWidget::resetAll() {
	++_loadRequestNum;
	_loadingChunk = false;
	_loadedCount = 0;
	_visibleTopItem = nullptr;
	_scrollDateLastItem = nullptr;
	_mouseActionItem = nullptr;
	_mouseAction = MouseAction::None;
	_selectedItem = nullptr;
	const auto ours = [&](Element *view) {
		return view && view->delegate().get() == this;
	};
	if (ours(Element::Hovered())) {
		Element::Hovered(nullptr);
	}
	if (ours(Element::HoveredLink())) {
		Element::HoveredLink(nullptr);
	}
	if (ours(Element::Pressed())) {
		Element::Pressed(nullptr);
	}
	if (ours(Element::PressedLink())) {
		Element::PressedLink(nullptr);
	}
	if (ours(Element::Moused())) {
		Element::Moused(nullptr);
	}
	_itemsByData.clear();
	_items.clear();
	_messageIds.clear();
	_itemDates.clear();
	_upLoaded = true;
	_downLoaded = true;
}

void InnerWidget::saveState(not_null<SectionMemento*> memento) {
	// Keep only how far we got: the messages are loaded again on return,
	// so nothing stale (or deleted) is ever carried over.
	memento->setItems({}, {}, true, false);
	memento->setLoadedCount(_loadedCount);
	memento->setSearchQuery(base::take(_searchQuery));
	resetAll();
}

void InnerWidget::restoreState(not_null<SectionMemento*> memento) {
	resetAll();
	_restoreCount = memento->loadedCount();
	_restoreScrollTop = memento->getScrollTop();
	_searchQuery = memento->takeSearchQuery();
	_upLoaded = true;
	_downLoaded = _ids.empty();
	updateMinMaxIds();
	updateEmptyText();
	updateSize();
	loadNextChunk();
}

void InnerWidget::applySearch(const QString &query) {
	if (true) {
		return;
	}
	if (_searchQuery != query) {
		++_loadRequestNum;
		_loadingUp = false;
		_loadingDown = false;

		_searchQuery = query;
		_upLoaded = false;
		_downLoaded = true;
		_items.clear();
		_messageIds.clear();
		_itemsByData.clear();
		_itemDates.clear();
		updateMinMaxIds();
		updateEmptyText();
		updateSize();
		preloadMore(Direction::Up);
	}
}

void InnerWidget::preloadMore(Direction direction) {
	// Results go top (best) to bottom, so more are loaded below as you
	// scroll down; nothing is ever above the first result.
	_upLoaded = true;
	if (direction == Direction::Up && _loadedCount > 0) {
		return;
	}
	loadNextChunk();
}

void InnerWidget::loadNextChunk() {
	constexpr auto kChunk = 100;
	if (_loadingChunk || _loadedCount >= int(_ids.size())) {
		_downLoaded = (_loadedCount >= int(_ids.size()));
		return;
	}
	_loadingChunk = true;
	const auto from = _loadedCount;
	const auto till = std::min(from + kChunk, int(_ids.size()));

	const auto reqNum = ++_loadRequestNum;
	const auto weak = base::make_weak(this);
	const auto left = std::make_shared<int>(0);
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=] {
		if (!weak || *finished || reqNum != _loadRequestNum) {
			return;
		}
		*finished = true;
		// Albums: the ranked id is one photo, so fetch the rest of each
		// album (consecutive ids, at most 10) before showing the chunk.
		auto &owner = session().data();
		auto seen = base::flat_set<MessageGroupId>();
		auto missing = std::vector<FullMsgId>();
		for (auto i = from; i != till; ++i) {
			const auto item = owner.message(_ids[i]);
			if (!item || !item->groupId() || seen.contains(item->groupId())) {
				continue;
			}
			seen.emplace(item->groupId());
			for (auto d = -9; d <= 9; ++d) {
				const auto id = item->id + d;
				if (d && id > 0 && !owner.message(item->history()->peer->id, id)) {
					missing.push_back({ item->history()->peer->id, id });
				}
			}
		}
		if (missing.empty()) {
			_loadingChunk = false;
			addRealMessages(from, till);
			return;
		}
		const auto albumLeft = std::make_shared<int>(int(missing.size()));
		const auto albumDone = std::make_shared<bool>(false);
		const auto show = [=] {
			if (!weak || *albumDone || reqNum != _loadRequestNum) {
				return;
			}
			*albumDone = true;
			_loadingChunk = false;
			addRealMessages(from, till);
		};
		for (const auto &id : missing) {
			session().api().requestMessageData(
				owner.peer(id.peer),
				id.msg,
				[=] {
					if (--*albumLeft <= 0) {
						show();
					}
				});
		}
		base::call_delayed(10000, this, show);
	};

	auto &owner = session().data();
	for (auto i = from; i != till; ++i) {
		if (!owner.message(_ids[i])) {
			++*left;
		}
	}
	if (!*left) {
		// Deferred, so a chunk is never added in the middle of a relayout.
		crl::on_main(this, finish);
		return;
	}
	for (auto i = from; i != till; ++i) {
		const auto id = _ids[i];
		if (owner.message(id)) {
			continue;
		}
		session().api().requestMessageData(
			owner.peer(id.peer),
			id.msg,
			[=] {
				if (--*left <= 0) {
					finish();
				}
			});
	}
	// Show whatever arrived even if some requests never answer.
	base::call_delayed(15000, this, [=] { finish(); });
}

void InnerWidget::addRealMessages(int from, int till) {
	const auto first = (_loadedCount == 0);
	_loadedCount = till;
	_upLoaded = true;
	_downLoaded = (_loadedCount >= int(_ids.size()));

	// _items.front() is drawn at the bottom: lower-ranked results go in
	// front of what is already there, the best of them closest to the top.
	auto &owner = session().data();
	auto added = std::vector<OwnedItem>();
	for (auto i = till - 1; i >= from; --i) {
		auto item = owner.message(_ids[i]);
		if (item) {
			// An album is drawn by its first item; the others are hidden.
			if (const auto group = owner.groups().find(item)) {
				item = group->items.front();
			}
		}
		if (!item || _itemsByData.find(item) != end(_itemsByData)) {
			continue;
		}
		auto owned = OwnedItem(this, item);
		_messageIds.emplace(uint64(_ids[i].msg.bare));
		_itemsByData.emplace(item, owned.get());
		added.push_back(std::move(owned));
	}
	const auto count = int(added.size());
	if (count) {
		for (auto &item : _items) {
			added.push_back(std::move(item));
		}
		_items = std::move(added);
		updateMinMaxIds();
		_upLoaded = true;
		itemsAdded(Direction::Down, count);
	}
	updateEmptyText();
	update();
	if (!_downLoaded && (!count || _loadedCount < _restoreCount)) {
		// Nothing usable in this chunk, or coming back to where we were.
		crl::on_main(this, [=] { loadNextChunk(); });
	} else if (_restoreScrollTop >= 0) {
		_scrollToSignal.fire(base::take(_restoreScrollTop));
		_restoreScrollTop = -1;
	} else if (first) {
		// Start at the top, on the most popular message.
		_scrollToSignal.fire(0);
	}
}

void InnerWidget::stepTo(int direction) {
	// Up / Down: jump so the previous / next message starts at the top.
	auto target = -1;
	for (const auto &item : ranges::views::reverse(_items)) {
		const auto top = itemTop(item.get());
		if (direction > 0 && top > _visibleTop + 2) {
			target = top;
			break;
		} else if (direction < 0 && top < _visibleTop - 2) {
			target = top;
		}
	}
	if (target >= 0) {
		_scrollToSignal.fire_copy(target);
	} else if (direction > 0) {
		loadNextChunk();
	}
}

std::vector<FullMsgId> InnerWidget::mediaOrder() const {
	return _ids;
}

void InnerWidget::removeRealItem(not_null<const HistoryItem*> item) {
	const auto i = _itemsByData.find(item);
	if (i == end(_itemsByData)) {
		return;
	}
	const auto view = i->second.get();
	_itemsByData.erase(i);
	if (_visibleTopItem == view) {
		_visibleTopItem = nullptr;
	}
	if (_scrollDateLastItem == view) {
		_scrollDateLastItem = nullptr;
	}
	if (_mouseActionItem == view) {
		_mouseActionItem = nullptr;
		_mouseAction = MouseAction::None;
	}
	if (_selectedItem == view) {
		_selectedItem = nullptr;
	}
	if (Element::Hovered() == view) {
		Element::Hovered(nullptr);
	}
	if (Element::HoveredLink() == view) {
		Element::HoveredLink(nullptr);
	}
	if (Element::Pressed() == view) {
		Element::Pressed(nullptr);
	}
	if (Element::PressedLink() == view) {
		Element::PressedLink(nullptr);
	}
	if (Element::Moused() == view) {
		Element::Moused(nullptr);
	}
	_items.erase(ranges::remove_if(_items, [&](const OwnedItem &owned) {
		return owned.get() == view;
	}), end(_items));
	updateSize();
}

void InnerWidget::updateMinMaxIds() {
	if (_messageIds.empty()) {
		_maxId = _minId = 0;
	} else {
		_maxId = *_messageIds.rbegin();
		_minId = *_messageIds.begin();
		if (_minId == 1) {
			_upLoaded = true;
		}
	}
}

void InnerWidget::itemsAdded(Direction direction, int addedCount) {
	Expects(addedCount >= 0);
	auto checkFrom = (direction == Direction::Up)
						 ? (_items.size() - addedCount)
						 : 1; // Should be ": 0", but zero is skipped anyway.
	auto checkTo = (direction == Direction::Up) ? (_items.size() + 1) : (addedCount + 1);
	for (auto i = checkFrom; i != checkTo; ++i) {
		if (i > 0) {
			const auto view = _items[i - 1].get();
			if (i < _items.size()) {
				const auto previous = _items[i].get();
				view->setDisplayDate(view->dateTime().date() != previous->dateTime().date());
				const auto attach = view->computeIsAttachToPrevious(previous);
				view->setAttachToPrevious(attach, previous);
				previous->setAttachToNext(attach, view);
			} else {
				view->setDisplayDate(true);
			}
		}
	}
	updateSize();
}

void InnerWidget::updateSize() {
	RpWidget::resizeToWidth(width());
	restoreScrollPosition();
	updateVisibleTopItem();
	checkPreloadMore();
}

int InnerWidget::resizeGetHeight(int newWidth) {
	update();

	const auto resizeAllItems = (_itemsWidth != newWidth);
	auto newHeight = 0;
	for (const auto &item : ranges::views::reverse(_items)) {
		item->setY(newHeight);
		if (item->pendingResize() || resizeAllItems) {
			newHeight += item->resizeGetHeight(newWidth);
		} else {
			newHeight += item->height();
		}
	}
	_itemsWidth = newWidth;
	_itemsHeight = newHeight;
	_itemsTop =
		(_minHeight > _itemsHeight + st::historyPaddingBottom)
			? (_minHeight - _itemsHeight - st::historyPaddingBottom)
			: 0;
	return _itemsTop + _itemsHeight + st::historyPaddingBottom;
}

void InnerWidget::restoreScrollPosition() {
	const auto newVisibleTop = _visibleTopItem
								   ? (itemTop(_visibleTopItem) + _visibleTopFromItem)
								   : ScrollMax;
	_scrollToSignal.fire_copy(newVisibleTop);
}

void InnerWidget::paintEvent(QPaintEvent *e) {
	if (_controller->contentOverlapped(this, e)) {
		return;
	}

	const auto guard = gsl::finally([&]
	{
		_userpicsCache.clear();
	});

	Painter p(this);

	auto clip = e->rect();
	auto context = _controller->preparePaintContext({
		.theme = _theme.get(),
		.clip = clip,
		.visibleAreaPositionGlobal = mapToGlobal(QPoint(0, _visibleTop)),
		.visibleAreaTop = _visibleTop,
		.visibleAreaWidth = width(),
	});
	if (_items.empty() && _upLoaded && _downLoaded) {
		paintEmpty(p, context.st);
	} else {
		_pathGradient->startFrame(
			0,
			width(),
			std::min(st::msgMaxWidth / 2, width() / 2));

		auto begin = std::rbegin(_items), end = std::rend(_items);
		auto from = std::lower_bound(begin,
									 end,
									 clip.top(),
									 [this](auto &elem, int top)
									 {
										 return this->itemTop(elem) + elem->height() <= top;
									 });
		auto to = std::lower_bound(begin,
								   end,
								   clip.top() + clip.height(),
								   [this](auto &elem, int bottom)
								   {
									   return this->itemTop(elem) < bottom;
								   });
		if (from != end) {
			auto top = itemTop(from->get());
			context.translate(0, -top);
			p.translate(0, top);
			for (auto i = from; i != to; ++i) {
				const auto view = i->get();
				context.outbg = view->hasOutLayout();
				context.selection = (view == _selectedItem)
										? _selectedText
										: TextSelection();
				view->draw(p, context);

				const auto height = view->height();
				top += height;
				context.translate(0, -height);
				p.translate(0, height);
			}
			context.translate(0, top);
			p.translate(0, -top);

			enumerateUserpics([&](not_null<Element*> view, int userpicTop)
			{
				// stop the enumeration if the userpic is below the painted rect
				if (userpicTop >= clip.top() + clip.height()) {
					return false;
				}

				// paint the userpic if it intersects the painted rect
				if (userpicTop + st::msgPhotoSize > clip.top()) {
					const auto from = view->data()->from();
					from->paintUserpicLeft(
						p,
						_userpics[from],
						st::historyPhotoLeft,
						userpicTop,
						view->width(),
						st::msgPhotoSize);
				}
				return true;
			});

			auto dateHeight = st::msgServicePadding.bottom() + st::msgServiceFont->height + st::msgServicePadding.top();
			auto scrollDateOpacity = _scrollDateOpacity.value(_scrollDateShown ? 1. : 0.);
			enumerateDates([&](not_null<Element*> view, int itemtop, int dateTop)
			{
				// stop the enumeration if the date is above the painted rect
				if (dateTop + dateHeight <= clip.top()) {
					return false;
				}

				const auto displayDate = view->displayDate();
				auto dateInPlace = displayDate;
				if (dateInPlace) {
					const auto correctDateTop = itemtop + st::msgServiceMargin.top();
					dateInPlace = (dateTop < correctDateTop + dateHeight);
				}
				//bool noFloatingDate = (item->date.date() == lastDate && displayDate);
				//if (noFloatingDate) {
				//	if (itemtop < showFloatingBefore) {
				//		noFloatingDate = false;
				//	}
				//}

				// paint the date if it intersects the painted rect
				if (dateTop < clip.top() + clip.height()) {
					auto opacity = (dateInPlace/* || noFloatingDate*/) ? 1. : scrollDateOpacity;
					if (opacity > 0.) {
						p.setOpacity(opacity);
						const auto dateY = /*noFloatingDate ? itemtop :*/
							(dateTop - st::msgServiceMargin.top());
						const auto width = view->width();
						if (const auto date = view->Get<HistoryView::DateBadge>()) {
							date->paint(p, context.st, dateY, width, true);
						} else {
							HistoryView::ServiceMessagePainter::PaintDate(
								p,
								context.st,
								view->dateTime(),
								dateY,
								width,
								true);
						}
					}
				}
				return true;
			});
		}
	}
}

auto InnerWidget::viewForItem(const HistoryItem *item) -> Element * {
	if (item) {
		const auto i = _itemsByData.find(item);
		if (i != _itemsByData.end()) {
			return i->second;
		}
	}
	return nullptr;
}

void InnerWidget::paintEmpty(Painter &p, not_null<const Ui::ChatStyle*> st) {
	auto rectWidth = st::historyAdminLogEmptyWidth;
	auto innerWidth = rectWidth - st::historyAdminLogEmptyPadding.left() - st::historyAdminLogEmptyPadding.right();
	auto rectHeight = st::historyAdminLogEmptyPadding.top() + _emptyText.countHeight(innerWidth)
		+ st::historyAdminLogEmptyPadding.bottom();
	auto rect = QRect((width() - rectWidth) / 2, (height() - rectHeight) / 3, rectWidth, rectHeight);
	HistoryView::ServiceMessagePainter::PaintBubble(p, st, rect);

	p.setPen(st->msgServiceFg());
	_emptyText.draw(p,
					rect.x() + st::historyAdminLogEmptyPadding.left(),
					rect.y() + st::historyAdminLogEmptyPadding.top(),
					innerWidth,
					style::al_top);
}

TextForMimeData InnerWidget::getSelectedText() const {
	return _selectedItem
			   ? _selectedItem->selectedText(_selectedText)
			   : TextForMimeData();
}

void InnerWidget::keyPressEvent(QKeyEvent *e) {
	const auto mods = e->modifiers() & ~Qt::KeypadModifier;
	if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)
		&& mods == Qt::NoModifier) {
		stepTo(e->key() == Qt::Key_Down ? 1 : -1);
		return;
	}
	if (e->key() == Qt::Key_Escape) {
		_controller->showBackFromStack();
	} else if (e == QKeySequence::Copy && _selectedItem != nullptr) {
		copySelectedText();
#ifdef Q_OS_MAC
		} else if (e->key() == Qt::Key_E && e->modifiers().testFlag(Qt::ControlModifier)) {
			TextUtilities::SetClipboardText(getSelectedText(), QClipboard::FindBuffer);
#endif // Q_OS_MAC
	} else {
		e->ignore();
	}
}

void InnerWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	mouseActionStart(e->globalPos(), e->button());
	if (((_mouseAction == MouseAction::Selecting && _selectedItem != nullptr) || (_mouseAction == MouseAction::None))
		&& _mouseSelectType == TextSelectType::Letters && _mouseActionItem) {
		StateRequest request;
		request.flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
		auto dragState = _mouseActionItem->textState(_dragStartPosition, request);
		if (dragState.cursor == CursorState::Text) {
			_mouseTextSymbol = dragState.symbol;
			_mouseSelectType = TextSelectType::Words;
			if (_mouseAction == MouseAction::None) {
				_mouseAction = MouseAction::Selecting;
				auto selection = TextSelection{dragState.symbol, dragState.symbol};
				repaintItem(std::exchange(_selectedItem, _mouseActionItem));
				_selectedText = selection;
			}
			mouseMoveEvent(e);

			_trippleClickPoint = e->globalPos();
			_trippleClickTimer.callOnce(QApplication::doubleClickInterval());
		}
	}
}

void InnerWidget::contextMenuEvent(QContextMenuEvent *e) {
	showContextMenu(e);
}

void InnerWidget::showContextMenu(QContextMenuEvent *e, bool showFromTouch) {
	if (e->reason() == QContextMenuEvent::Mouse) {
		mouseActionUpdate(e->globalPos());
	}

	// -1 - has selection, but no over, 0 - no selection, 1 - over text
	auto isUponSelected = 0;
	auto hasSelected = 0;
	if (_selectedItem) {
		isUponSelected = -1;

		auto selFrom = _selectedText.from;
		auto selTo = _selectedText.to;
		hasSelected = (selTo > selFrom) ? 1 : 0;
		if (Element::Moused() && Element::Moused() == Element::Hovered()) {
			auto mousePos = mapPointToItem(
				mapFromGlobal(_mousePosition),
				Element::Moused());
			StateRequest request;
			request.flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
			auto dragState = Element::Moused()->textState(mousePos, request);
			if (dragState.cursor == CursorState::Text
				&& base::in_range(dragState.symbol, selFrom, selTo)) {
				isUponSelected = 1;
			}
		}
	}
	if (showFromTouch && hasSelected && isUponSelected < hasSelected) {
		isUponSelected = hasSelected;
	}

	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuExpandedSeparator);

	if (const auto target = Element::Hovered()
			? Element::Hovered()
			: Element::HoveredLink()) {
		const auto itemId = target->data()->fullId();
		const auto controller = _controller;
		const auto topicRootId = target->data()->topicRootId();
		const auto peer = target->data()->history()->peer;
		_menu->addAction(u"Go to message"_q, [=] {
			const auto params = Window::SectionShow(
				Window::SectionShow::Way::Forward);
			if (topicRootId && peer->forum()) {
				if (const auto topic = peer->forumTopicFor(topicRootId)) {
					controller->showTopic(topic, itemId.msg, params);
					return;
				}
			}
			controller->showPeerHistory(peer, params, itemId.msg);
		}, &st::menuIconShowInChat);
	}

	const auto link = ClickHandler::getActive();
	auto view = Element::Hovered()
					? Element::Hovered()
					: Element::HoveredLink();
	const auto lnkPhoto = link
							  ? reinterpret_cast<PhotoData*>(
								  link->property(kPhotoLinkMediaProperty).toULongLong())
							  : nullptr;
	const auto lnkDocument = link
								 ? reinterpret_cast<DocumentData*>(
									 link->property(kDocumentLinkMediaProperty).toULongLong())
								 : nullptr;
	auto lnkIsVideo = lnkDocument ? lnkDocument->isVideoFile() : false;
	auto lnkIsVoice = lnkDocument ? lnkDocument->isVoiceMessage() : false;
	auto lnkIsAudio = lnkDocument ? lnkDocument->isAudioFile() : false;
	const auto fromId = PeerId(link
								   ? link->property(kPeerLinkPeerIdProperty).toULongLong()
								   : 0);
	if (lnkPhoto || lnkDocument) {
		if (isUponSelected > 0) {
			_menu->addAction(tr::lng_context_copy_selected(tr::now),
							 [=]
							 {
								 copySelectedText();
							 },
							 &st::menuIconCopy);
		}
		if (lnkPhoto) {
			const auto media = lnkPhoto->activeMediaView();
			if (!lnkPhoto->isNull() && media && media->loaded()) {
				_menu->addAction(tr::lng_context_save_image(tr::now),
								 base::fn_delayed(st::defaultDropdownMenu.menu.ripple.hideDuration,
												  this,
												  [=]
												  {
													  savePhotoToFile(lnkPhoto);
												  }),
								 &st::menuIconSaveImage);
				_menu->addAction(tr::lng_context_copy_image(tr::now),
								 [=]
								 {
									 copyContextImage(lnkPhoto);
								 },
								 &st::menuIconCopy);
			}
			if (lnkPhoto->hasAttachedStickers()) {
				const auto controller = _controller;
				auto callback = [=]
				{
					auto &attached = session().api().attachedStickers();
					attached.requestAttachedStickerSets(controller, lnkPhoto);
				};
				_menu->addAction(
					tr::lng_context_attached_stickers(tr::now),
					std::move(callback),
					&st::menuIconStickers);
			}
		} else {
			if (lnkDocument->loading()) {
				_menu->addAction(tr::lng_context_cancel_download(tr::now),
								 [=]
								 {
									 cancelContextDownload(lnkDocument);
								 },
								 &st::menuIconCancel);
			} else {
				const auto itemId = view
										? view->data()->fullId()
										: FullMsgId();
				if (const auto item = session().data().message(itemId)) {
					const auto notAutoplayedGif = [&]
					{
						return lnkDocument->isGifv()
							&& !Data::AutoDownload::ShouldAutoPlay(
								session().settings().autoDownload(),
								item->history()->peer,
								lnkDocument);
					}();
					if (notAutoplayedGif) {
						_menu->addAction(tr::lng_context_open_gif(tr::now),
										 [=]
										 {
											 openContextGif(itemId);
										 },
										 &st::menuIconShowInChat);
					}
				}
				if (!lnkDocument->filepath(true).isEmpty()) {
					_menu->addAction(Platform::IsMac()
										 ? tr::lng_context_show_in_finder(tr::now)
										 : tr::lng_context_show_in_folder(tr::now),
									 [=]
									 {
										 showContextInFolder(lnkDocument);
									 },
									 &st::menuIconShowInFolder);
				}
				_menu->addAction(lnkIsVideo
									 ? tr::lng_context_save_video(tr::now)
									 : (lnkIsVoice
											? tr::lng_context_save_audio(tr::now)
											: (lnkIsAudio
												   ? tr::lng_context_save_audio_file(
													   tr::now)
												   : tr::lng_context_save_file(tr::now))),
								 base::fn_delayed(st::defaultDropdownMenu.menu.ripple.hideDuration,
												  this,
												  [this, lnkDocument]
												  {
													  saveDocumentToFile(lnkDocument);
												  }),
								 &st::menuIconDownload);

				HistoryView::AddCopyFilename(
					_menu,
					lnkDocument,
					[] { return false; });


				if (lnkDocument->hasAttachedStickers()) {
					const auto controller = _controller;
					auto callback = [=]
					{
						auto &attached = session().api().attachedStickers();
						attached.requestAttachedStickerSets(controller, lnkDocument);
					};
					_menu->addAction(
						tr::lng_context_attached_stickers(tr::now),
						std::move(callback),
						&st::menuIconStickers);
				}
			}
		}
	} else if (fromId) {
		// suggest to block
		//		if (const auto participant = session().data().peer(fromId)) {
		//			suggestRestrictParticipant(participant);
		//		}
	} else {
		// maybe cursor on some text history item?
		const auto item = view ? view->data().get() : nullptr;
		const auto itemId = item ? item->fullId() : FullMsgId();

		if (isUponSelected > 0) {
			_menu->addAction(
				tr::lng_context_copy_selected(tr::now),
				[this]
				{
					copySelectedText();
				},
				&st::menuIconCopy);
		} else {
			if (item && !isUponSelected) {
				const auto media = view->media();
				const auto mediaHasTextForCopy = media && media->hasTextForCopy();
				if (const auto document = media ? media->getDocument() : nullptr) {
					if (document->sticker()) {
						_menu->addAction(tr::lng_context_save_image(tr::now),
										 base::fn_delayed(st::defaultDropdownMenu.menu.ripple.hideDuration,
														  this,
														  [this, document]
														  {
															  saveDocumentToFile(document);
														  }),
										 &st::menuIconDownload);
					}
				}
				if (!item->isService()
					&& !link
					&& (view->hasVisibleText()
						|| mediaHasTextForCopy
						|| item->Has<HistoryMessageLogEntryOriginal>())) {
					_menu->addAction(tr::lng_context_copy_text(tr::now),
									 [=]
									 {
										 copyContextText(itemId);
									 },
									 &st::menuIconCopy);
				}
			}
		}

		const auto actionText = link
									? link->copyToClipboardContextItemText()
									: QString();
		if (!actionText.isEmpty()) {
			_menu->addAction(
				actionText,
				[text = link->copyToClipboardText()]
				{
					QGuiApplication::clipboard()->setText(text);
				},
				&st::menuIconCopy);
		}
	}

	if (_menu->empty()) {
		_menu = nullptr;
	} else {
		_menu->popup(e->globalPos());
		e->accept();
	}
}

void InnerWidget::savePhotoToFile(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}

	auto filter = u"JPEG Image (*.jpg);;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetWritePath(
		this,
		tr::lng_save_photo(tr::now),
		filter,
		filedialogDefaultName(u"photo"_q, u".jpg"_q),
		crl::guard(this,
				   [=](const QString &result)
				   {
					   if (!result.isEmpty()) {
						   media->saveToFile(result);
					   }
				   }));
}

void InnerWidget::saveDocumentToFile(not_null<DocumentData*> document) {
	DocumentSaveClickHandler::Save(
		Data::FileOrigin(),
		document,
		DocumentSaveClickHandler::Mode::ToNewFile);
}

void InnerWidget::copyContextImage(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}
	media->setToClipboard();
}

void InnerWidget::copySelectedText() {
	TextUtilities::SetClipboardText(getSelectedText());
}

void InnerWidget::showStickerPackInfo(not_null<DocumentData*> document) {
	StickerSetBox::Show(_controller->uiShow(), document);
}

void InnerWidget::cancelContextDownload(not_null<DocumentData*> document) {
	document->cancel();
}

void InnerWidget::showContextInFolder(not_null<DocumentData*> document) {
	const auto filepath = document->filepath(true);
	if (!filepath.isEmpty()) {
		File::ShowInFolder(filepath);
	}
}

void InnerWidget::openContextGif(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				_controller->openDocument(document, true, {itemId});
			}
		}
	}
}

void InnerWidget::copyContextText(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		TextUtilities::SetClipboardText(HistoryItemText(item));
	}
}

void InnerWidget::mousePressEvent(QMouseEvent *e) {
	if (_menu) {
		e->accept();
		return; // ignore mouse press, that was hiding context menu
	}
	mouseActionStart(e->globalPos(), e->button());
}

void InnerWidget::mouseMoveEvent(QMouseEvent *e) {
	auto buttonsPressed = (e->buttons() & (Qt::LeftButton | Qt::MiddleButton));
	if (!buttonsPressed && _mouseAction != MouseAction::None) {
		mouseReleaseEvent(e);
	}
	mouseActionUpdate(e->globalPos());
}

void InnerWidget::mouseReleaseEvent(QMouseEvent *e) {
	mouseActionFinish(e->globalPos(), e->button());
	if (!rect().contains(e->pos())) {
		leaveEvent(e);
	}
}

void InnerWidget::enterEventHook(QEnterEvent *e) {
	mouseActionUpdate(QCursor::pos());
	return RpWidget::enterEventHook(e);
}

void InnerWidget::leaveEventHook(QEvent *e) {
	if (const auto view = Element::Hovered()) {
		repaintItem(view);
		Element::Hovered(nullptr);
	}
	ClickHandler::clearActive();
	Ui::Tooltip::Hide();
	if (!ClickHandler::getPressed() && _cursor != style::cur_default) {
		_cursor = style::cur_default;
		setCursor(_cursor);
	}
	return RpWidget::leaveEventHook(e);
}

void InnerWidget::mouseActionStart(const QPoint &screenPos, Qt::MouseButton button) {
	mouseActionUpdate(screenPos);
	if (button != Qt::LeftButton) return;

	ClickHandler::pressed();
	if (Element::Pressed() != Element::Hovered()) {
		repaintItem(Element::Pressed());
		Element::Pressed(Element::Hovered());
		repaintItem(Element::Pressed());
	}

	_mouseAction = MouseAction::None;
	_mouseActionItem = Element::Moused();
	_dragStartPosition = mapPointToItem(
		mapFromGlobal(screenPos),
		_mouseActionItem);
	_pressWasInactive = Ui::WasInactivePress(_controller->widget());
	if (_pressWasInactive) {
		Ui::MarkInactivePress(_controller->widget(), false);
	}

	if (ClickHandler::getPressed()) {
		_mouseAction = MouseAction::PrepareDrag;
	}
	if (_mouseAction == MouseAction::None && _mouseActionItem) {
		TextState dragState;
		if (_trippleClickTimer.isActive()
			&& (screenPos - _trippleClickPoint).manhattanLength() < QApplication::startDragDistance()) {
			StateRequest request;
			request.flags = Ui::Text::StateRequest::Flag::LookupSymbol;
			dragState = _mouseActionItem->textState(_dragStartPosition, request);
			if (dragState.cursor == CursorState::Text) {
				auto selection = TextSelection{dragState.symbol, dragState.symbol};
				repaintItem(std::exchange(_selectedItem, _mouseActionItem));
				_selectedText = selection;
				_mouseTextSymbol = dragState.symbol;
				_mouseAction = MouseAction::Selecting;
				_mouseSelectType = TextSelectType::Paragraphs;
				mouseActionUpdate(_mousePosition);
				_trippleClickTimer.callOnce(QApplication::doubleClickInterval());
			}
		} else if (Element::Pressed()) {
			StateRequest request;
			request.flags = Ui::Text::StateRequest::Flag::LookupSymbol;
			dragState = _mouseActionItem->textState(_dragStartPosition, request);
		}
		if (_mouseSelectType != TextSelectType::Paragraphs) {
			if (Element::Pressed()) {
				_mouseTextSymbol = dragState.symbol;
				auto uponSelected = (dragState.cursor == CursorState::Text);
				if (uponSelected) {
					if (!_selectedItem || _selectedItem != _mouseActionItem) {
						uponSelected = false;
					} else if (_mouseTextSymbol < _selectedText.from || _mouseTextSymbol >= _selectedText.to) {
						uponSelected = false;
					}
				}
				if (uponSelected) {
					_mouseAction = MouseAction::PrepareDrag; // start text drag
				} else if (!_pressWasInactive) {
					if (dragState.afterSymbol) ++_mouseTextSymbol;
					auto selection = TextSelection{_mouseTextSymbol, _mouseTextSymbol};
					repaintItem(std::exchange(_selectedItem, _mouseActionItem));
					_selectedText = selection;
					_mouseAction = MouseAction::Selecting;
					repaintItem(_mouseActionItem);
				}
			}
		}
	}

	if (!_mouseActionItem) {
		_mouseAction = MouseAction::None;
	} else if (_mouseAction == MouseAction::None) {
		_mouseActionItem = nullptr;
	}
}

void InnerWidget::mouseActionUpdate(const QPoint &screenPos) {
	_mousePosition = screenPos;
	updateSelected();
}

void InnerWidget::mouseActionCancel() {
	_mouseActionItem = nullptr;
	_mouseAction = MouseAction::None;
	_dragStartPosition = QPoint(0, 0);
	_wasSelectedText = false;
}

void InnerWidget::mouseActionFinish(const QPoint &screenPos, Qt::MouseButton button) {
	mouseActionUpdate(screenPos);

	auto activated = ClickHandler::unpressed();
	if (_mouseAction == MouseAction::Dragging) {
		activated = nullptr;
	}
	if (const auto view = Element::Pressed()) {
		repaintItem(view);
		Element::Pressed(nullptr);
	}

	_wasSelectedText = false;

	if (activated) {
		mouseActionCancel();
		ActivateClickHandler(window(),
							 activated,
							 {
								 button,
								 QVariant::fromValue(ClickHandlerContext{
									 .elementDelegate = [weak = base::make_weak(this)]
									 {
										 return weak.get();
									 },
									 .sessionWindow = base::make_weak(_controller),
								 })
							 });
		return;
	}
	if (_mouseAction == MouseAction::PrepareDrag && !_pressWasInactive && button != Qt::RightButton) {
		repaintItem(base::take(_selectedItem));
	} else if (_mouseAction == MouseAction::Selecting) {
		if (_selectedItem && !_pressWasInactive) {
			if (_selectedText.from == _selectedText.to) {
				_selectedItem = nullptr;
				_controller->widget()->setInnerFocus();
			}
		}
	}
	_mouseAction = MouseAction::None;
	_mouseActionItem = nullptr;
	_mouseSelectType = TextSelectType::Letters;
	//_widget->noSelectingScroll(); // TODO

	if (QGuiApplication::clipboard()->supportsSelection()
		&& _selectedItem
		&& _selectedText.from != _selectedText.to) {
		TextUtilities::SetClipboardText(
			_selectedItem->selectedText(_selectedText),
			QClipboard::Selection);
	}
}

void InnerWidget::updateSelected() {
	auto mousePosition = mapFromGlobal(_mousePosition);
	auto point = QPoint(
		std::clamp(mousePosition.x(), 0, width()),
		std::clamp(mousePosition.y(), _visibleTop, _visibleBottom));

	auto itemPoint = QPoint();
	auto begin = std::rbegin(_items), end = std::rend(_items);
	auto from = (point.y() >= _itemsTop && point.y() < _itemsTop + _itemsHeight)
					? std::lower_bound(begin,
									   end,
									   point.y(),
									   [this](auto &elem, int top)
									   {
										   return this->itemTop(elem) + elem->height() <= top;
									   })
					: end;
	const auto view = (from != end) ? from->get() : nullptr;
	const auto item = view ? view->data().get() : nullptr;
	if (item) {
		Element::Moused(view);
		itemPoint = mapPointToItem(point, view);
		if (view->pointState(itemPoint) != PointState::Outside) {
			if (Element::Hovered() != view) {
				repaintItem(Element::Hovered());
				Element::Hovered(view);
				repaintItem(view);
			}
		} else if (const auto view = Element::Hovered()) {
			repaintItem(view);
			Element::Hovered(nullptr);
		}
	}

	TextState dragState;
	ClickHandlerHost *lnkhost = nullptr;
	auto selectingText = _selectedItem
		&& (view == _mouseActionItem)
		&& (view == Element::Hovered());
	if (view) {
		if (view != _mouseActionItem
			|| (itemPoint - _dragStartPosition).manhattanLength() >= QApplication::startDragDistance()) {
			if (_mouseAction == MouseAction::PrepareDrag) {
				_mouseAction = MouseAction::Dragging;
				InvokeQueued(this,
							 [this]
							 {
								 performDrag();
							 });
			}
		}
		StateRequest request;
		if (_mouseAction == MouseAction::Selecting) {
			request.flags |= Ui::Text::StateRequest::Flag::LookupSymbol;
		} else {
			selectingText = false;
		}
		if (base::IsAltPressed()) {
			request.flags &= ~Ui::Text::StateRequest::Flag::LookupLink;
		}
		dragState = view->textState(itemPoint, request);
		lnkhost = view;
		if (!dragState.link && itemPoint.x() >= st::historyPhotoLeft
			&& itemPoint.x() < st::historyPhotoLeft + st::msgPhotoSize) {
			if (!item->isService() && view->hasFromPhoto()) {
				enumerateUserpics([&](not_null<Element*> view, int userpicTop)
				{
					// stop enumeration if the userpic is below our point
					if (userpicTop > point.y()) {
						return false;
					}

					// stop enumeration if we've found a userpic under the cursor
					if (point.y() >= userpicTop && point.y() < userpicTop + st::msgPhotoSize) {
						dragState.link = view->data()->from()->openLink();
						lnkhost = view;
						return false;
					}
					return true;
				});
			}
		}
	}
	auto lnkChanged = ClickHandler::setActive(dragState.link, lnkhost);
	if (lnkChanged || dragState.cursor != _mouseCursorState) {
		Ui::Tooltip::Hide();
	}
	if (dragState.link
		|| dragState.cursor == CursorState::Date
		|| dragState.cursor == CursorState::Forwarded) {
		Ui::Tooltip::Show(350, this);
	}

	auto cursor = style::cur_default;
	if (_mouseAction == MouseAction::None) {
		_mouseCursorState = dragState.cursor;
		if (dragState.link) {
			cursor = style::cur_pointer;
		} else if (_mouseCursorState == CursorState::Text) {
			cursor = style::cur_text;
		} else if (_mouseCursorState == CursorState::Date) {
			//			cursor = style::cur_cross;
		}
	} else if (item) {
		if (_mouseAction == MouseAction::Selecting) {
			if (selectingText) {
				auto second = dragState.symbol;
				if (dragState.afterSymbol && _mouseSelectType == TextSelectType::Letters) {
					++second;
				}
				auto selection = TextSelection{qMin(second, _mouseTextSymbol), qMax(second, _mouseTextSymbol)};
				if (_mouseSelectType != TextSelectType::Letters) {
					selection = _mouseActionItem->adjustSelection(
						selection,
						_mouseSelectType);
				}
				if (_selectedText != selection) {
					_selectedText = selection;
					repaintItem(_mouseActionItem);
				}
				if (!_wasSelectedText && (selection.from != selection.to)) {
					_wasSelectedText = true;
					setFocus();
				}
			}
		} else if (_mouseAction == MouseAction::Dragging) {
		}

		if (ClickHandler::getPressed()) {
			cursor = style::cur_pointer;
		} else if (_mouseAction == MouseAction::Selecting && _selectedItem) {
			cursor = style::cur_text;
		}
	}

	// Voice message seek support.
	if (const auto pressedView = Element::PressedLink()) {
		const auto adjustedPoint = mapPointToItem(point, pressedView);
		pressedView->updatePressed(adjustedPoint);
	}

	//if (_mouseAction == MouseAction::Selecting) {
	//	_widget->checkSelectingScroll(mousePos);
	//} else {
	//	_widget->noSelectingScroll();
	//} // TODO

	if (_mouseAction == MouseAction::None && (lnkChanged || cursor != _cursor)) {
		setCursor(_cursor = cursor);
	}
}

void InnerWidget::performDrag() {
	if (_mouseAction != MouseAction::Dragging) return;
}

int InnerWidget::itemTop(not_null<const Element*> view) const {
	return _itemsTop + view->y();
}

void InnerWidget::repaintItem(const Element *view) {
	if (!view) {
		return;
	}
	const auto top = itemTop(view);
	const auto range = view->verticalRepaintRange();
	update(0, top + range.top, width(), range.height);
}

void InnerWidget::resizeItem(not_null<Element*> view) {
	updateSize();
}

void InnerWidget::refreshItem(not_null<const Element*> view) {
	// No need to refresh views in admin log.
	// sogl
}

QPoint InnerWidget::mapPointToItem(QPoint point, const Element *view) const {
	if (!view) {
		return QPoint();
	}
	return point - QPoint(0, itemTop(view));
}

InnerWidget::~InnerWidget() = default;

} // namespace TopMessagesView
