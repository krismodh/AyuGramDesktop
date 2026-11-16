// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/top_messages_box.h"
#include "ayu/ui/top_messages_view/tm_section.h"

#include "apiwrap.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "mainwidget.h"
#include "dialogs/dialogs_key.h"
#include "ui/painter.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/standalone_layer_stack.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>
#include <QtGui/QTextDocument>
#include <QtGui/QKeyEvent>

namespace {

constexpr auto kPerRequest = 100;
constexpr auto kRequestDelay = crl::time(350);
constexpr auto kFloodExtraDelay = crl::time(500);
constexpr auto kShownLimit = 100;
constexpr auto kPreviewLength = 90;
constexpr auto kRefreshEvery = 1000;
constexpr auto kCachedScans = 5;
constexpr auto kFileMagic = qint32(0x544D5331);
constexpr auto kLimits = std::array{
	500, 1000, 2000, 5000, 10000, 25000, 50000, 100000, 0,
};
constexpr auto kSkips = std::array{
	0, 500, 1000, 2000, 5000, 10000, 25000, 50000, 100000,
};
constexpr auto kDefaultLimitIndex = 3;

enum class SortBy {
	Reactions,
	Replies,
	Forwards,
	Views,
};

struct Entry {
	MsgId id = 0;
	MsgId topicRoot = 0;
	TimeId date = 0;
	PeerId from = 0;
	QString text;
	int reactions = 0;
	int replies = 0;
	int forwards = 0;
	int views = 0;
	bool media = false;
};

// Shared with the cache, so reopening the window shows the last scan.
struct Results {
	std::vector<Entry> entries;
	SortBy sort = SortBy::Reactions;
	bool mediaOnly = false;
	int skip = 0;
	int limit = kLimits[kDefaultLimitIndex];
	MsgId offsetId = 0;
	int scanned = 0;
	bool reachedStart = false;
	bool hasScan = false;
};

struct State {
	~State() {
		if (save) {
			save();
		}
	}

	std::shared_ptr<Results> results;
	Fn<void()> save;
	bool dirty = false;
	int sinceRefresh = 0;
	int page = 0;
	bool scrollToTop = false;
	bool scanning = false;
	mtpRequestId requestId = 0;
	base::Timer timer;
	Fn<void()> requestNext;
	Fn<void()> refresh;
	rpl::variable<QString> status;
	rpl::variable<QString> buttonText = u"Scan"_q;
};

using CacheKey = std::pair<PeerId, MsgId>;

struct OpenWindow {
	not_null<Ui::StandaloneLayerStack*> stack;
	CacheKey key;
};

[[nodiscard]] auto &Cache() {
	static auto result = std::vector<
		std::pair<CacheKey, std::shared_ptr<Results>>>();
	return result;
}

[[nodiscard]] auto &Windows() {
	static auto result = base::flat_map<
		not_null<Window::SessionController*>,
		OpenWindow>();
	return result;
}

[[nodiscard]] std::shared_ptr<Results> CachedResults(CacheKey key) {
	for (const auto &[cached, results] : Cache()) {
		if (cached == key) {
			return results;
		}
	}
	return nullptr;
}

void RememberResults(CacheKey key, std::shared_ptr<Results> results) {
	auto &cache = Cache();
	cache.erase(ranges::remove(cache, key, [](const auto &pair) {
		return pair.first;
	}), end(cache));
	cache.insert(begin(cache), { key, std::move(results) });
	if (int(cache.size()) > kCachedScans) {
		cache.resize(kCachedScans);
	}
}

[[nodiscard]] QString CachePath(
		not_null<Main::Session*> session,
		CacheKey key) {
	return cWorkingDir()
		+ u"tdata/ayu/top_messages/"_q
		+ QString::number(session->userId().bare)
		+ u"_"_q
		+ QString::number(key.first.value)
		+ u"_"_q
		+ QString::number(key.second.bare)
		+ u".bin"_q;
}

void SaveResults(const QString &path, const Results &results) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	stream
		<< kFileMagic
		<< qint32(results.sort)
		<< results.mediaOnly
		<< qint32(results.skip)
		<< qint32(results.limit)
		<< qint64(results.offsetId.bare)
		<< qint32(results.scanned)
		<< results.reachedStart
		<< qint32(results.entries.size());
	for (const auto &entry : results.entries) {
		stream
			<< qint64(entry.id.bare)
			<< qint64(entry.topicRoot.bare)
			<< qint32(entry.date)
			<< quint64(entry.from.value)
			<< entry.text
			<< qint32(entry.reactions)
			<< qint32(entry.replies)
			<< qint32(entry.forwards)
			<< qint32(entry.views)
			<< entry.media;
	}
	if (stream.status() == QDataStream::Ok) {
		file.commit();
	}
}

[[nodiscard]] std::shared_ptr<Results> LoadResults(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return nullptr;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	auto magic = qint32();
	auto sort = qint32();
	auto skip = qint32();
	auto limit = qint32();
	auto offsetId = qint64();
	auto scanned = qint32();
	auto count = qint32();
	auto result = std::make_shared<Results>();
	stream >> magic;
	if (magic != kFileMagic) {
		return nullptr;
	}
	stream
		>> sort
		>> result->mediaOnly
		>> skip
		>> limit
		>> offsetId
		>> scanned
		>> result->reachedStart
		>> count;
	if (stream.status() != QDataStream::Ok
		|| sort < 0
		|| sort > int(SortBy::Views)
		|| count < 0) {
		return nullptr;
	}
	result->sort = SortBy(sort);
	result->skip = skip;
	result->limit = limit;
	result->offsetId = MsgId(offsetId);
	result->scanned = scanned;
	result->hasScan = true;
	result->entries.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto entry = Entry();
		auto id = qint64();
		auto topicRoot = qint64();
		auto date = qint32();
		auto from = quint64();
		auto reactions = qint32();
		auto replies = qint32();
		auto forwards = qint32();
		auto views = qint32();
		stream
			>> id
			>> topicRoot
			>> date
			>> from
			>> entry.text
			>> reactions
			>> replies
			>> forwards
			>> views
			>> entry.media;
		if (stream.status() != QDataStream::Ok) {
			return nullptr;
		}
		entry.id = MsgId(id);
		entry.topicRoot = MsgId(topicRoot);
		entry.date = TimeId(date);
		entry.from = PeerId(PeerIdHelper(from));
		entry.reactions = reactions;
		entry.replies = replies;
		entry.forwards = forwards;
		entry.views = views;
		result->entries.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] QString FormatNumber(int value) {
	return QLocale().toString(value);
}

[[nodiscard]] QString LimitText(int limit) {
	return limit
		? (FormatNumber(limit) + u" messages"_q)
		: u"All the rest"_q;
}

[[nodiscard]] QString SkipText(int skip) {
	return skip
		? (u"Skip newest "_q + FormatNumber(skip))
		: u"Newest message"_q;
}

[[nodiscard]] QString RangeText(int skip, int limit) {
	return u"Range: messages "_q
		+ FormatNumber(skip)
		+ QString::fromUtf8(" \xE2\x80\x93 ")
		+ (limit ? FormatNumber(skip + limit) : u"start of chat"_q)
		+ u" (counting back from the newest)."_q;
}

[[nodiscard]] int Metric(const Entry &entry, SortBy sort) {
	switch (sort) {
	case SortBy::Reactions: return entry.reactions;
	case SortBy::Replies: return entry.replies;
	case SortBy::Forwards: return entry.forwards;
	case SortBy::Views: return entry.views;
	}
	return 0;
}

[[nodiscard]] MsgId TopicRoot(const MTPDmessage &data) {
	auto result = MsgId(Data::ForumTopic::kGeneralId);
	if (const auto reply = data.vreply_to()) {
		reply->match([&](const MTPDmessageReplyHeader &header) {
			if (header.is_forum_topic()) {
				result = MsgId(header.vreply_to_top_id().value_or(
					header.vreply_to_msg_id().value_or_empty()));
			}
		}, [](const auto &) {
		});
	}
	return result;
}

void ParseMessages(
		const QVector<MTPMessage> &list,
		not_null<Results*> results,
		bool forum,
		MsgId scopeRoot) {
	for (const auto &message : list) {
		message.match([&](const MTPDmessage &data) {
			auto entry = Entry();
			entry.id = MsgId(data.vid().v);
			entry.topicRoot = scopeRoot
				? scopeRoot
				: forum
				? TopicRoot(data)
				: MsgId(0);
			entry.date = data.vdate().v;
			if (const auto from = data.vfrom_id()) {
				entry.from = peerFromMTP(*from);
			}
			entry.text = qs(data.vmessage()).left(kPreviewLength);
			entry.text.replace(QChar('\n'), QChar(' '));
			if (const auto reactions = data.vreactions()) {
				for (const auto &count : reactions->data().vresults().v) {
					entry.reactions += count.data().vcount().v;
				}
			}
			if (const auto replies = data.vreplies()) {
				entry.replies = replies->data().vreplies().v;
			}
			entry.forwards = data.vforwards().value_or_empty();
			entry.views = data.vviews().value_or_empty();
			if (const auto media = data.vmedia()) {
				entry.media = (media->type() != mtpc_messageMediaEmpty)
					&& (media->type() != mtpc_messageMediaWebPage);
			}
			results->entries.push_back(std::move(entry));
			results->offsetId = MsgId(data.vid().v);
		}, [&](const MTPDmessageService &data) {
			results->offsetId = MsgId(data.vid().v);
		}, [&](const MTPDmessageEmpty &data) {
			results->offsetId = MsgId(data.vid().v);
		});
	}
}


[[nodiscard]] QString SortName(SortBy sort) {
	switch (sort) {
	case SortBy::Reactions: return u"reactions"_q;
	case SortBy::Replies: return u"replies"_q;
	case SortBy::Forwards: return u"forwards"_q;
	case SortBy::Views: return u"views"_q;
	}
	return QString();
}

void JumpToResult(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId id,
		MsgId topicRoot) {
	const auto params = Window::SectionShow(
		Window::SectionShow::Way::ClearStack);
	if (topicRoot && peer->forum()) {
		if (const auto topic = peer->forumTopicFor(topicRoot)) {
			controller->showTopic(topic, id, params);
		} else {
			peer->forum()->requestTopic(topicRoot, crl::guard(controller.get(), [=] {
				if (const auto topic = peer->forumTopicFor(topicRoot)) {
					controller->showTopic(topic, id, params);
				}
			}));
		}
		return;
	}
	controller->showPeerHistory(peer, params.way, id);
}

struct NavItem {
	MsgId id = 0;
	MsgId topicRoot = 0;
	int metric = 0;
};

// Floating "3 of 250  ^ v" bar at the bottom of the chat, like in-chat search.
class ResultsBar final : public Ui::RpWidget {
public:
	ResultsBar(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		SortBy sort,
		std::vector<NavItem> items,
		int index);

	void go(int index);

protected:
	bool eventFilter(QObject *o, QEvent *e) override;

private:
	[[nodiscard]] bool arrowsAllowed() const;
	void updateText();
	void relayout();
	void updatePlacement();
	[[nodiscard]] bool inOurChat() const;

	const not_null<Window::SessionController*> _controller;
	const not_null<PeerData*> _peer;
	const MsgId _topicRootId;
	const SortBy _sort;
	const std::vector<NavItem> _items;
	int _index = 0;

	const not_null<Ui::FlatLabel*> _title;
	const not_null<Ui::FlatLabel*> _counter;
	const not_null<Ui::LinkButton*> _list;
	const not_null<Ui::IconButton*> _previous;
	const not_null<Ui::IconButton*> _next;
	const not_null<Ui::IconButton*> _close;
	base::Timer _placeTimer;

};

[[nodiscard]] auto &Bars() {
	static auto result = base::flat_map<
		not_null<Window::SessionController*>,
		base::unique_qptr<ResultsBar>>();
	return result;
}

ResultsBar::ResultsBar(
	not_null<Ui::RpWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId,
	SortBy sort,
	std::vector<NavItem> items,
	int index)
: RpWidget(parent)
, _controller(controller)
, _peer(peer)
, _topicRootId(topicRootId)
, _sort(sort)
, _items(std::move(items))
, _title(Ui::CreateChild<Ui::FlatLabel>(
	this,
	u"Top messages"_q,
	st::boxDividerLabel))
, _counter(Ui::CreateChild<Ui::FlatLabel>(
	this,
	QString(),
	st::defaultSettingsRightLabel))
, _list(Ui::CreateChild<Ui::LinkButton>(this, u"List"_q))
// Same icons (and swap) as the in-chat search bar: up = higher rank.
, _previous(Ui::CreateChild<Ui::IconButton>(this, st::calendarNext))
, _next(Ui::CreateChild<Ui::IconButton>(this, st::calendarPrevious))
, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
, _placeTimer([=] { updatePlacement(); }) {
	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_counter->setAttribute(Qt::WA_TransparentForMouseEvents);

	_previous->setClickedCallback([=] { go(_index - 1); });
	_next->setClickedCallback([=] { go(_index + 1); });
	_list->setClickedCallback([=] {
		const auto controller = _controller;
		const auto peer = _peer;
		const auto root = _topicRootId;
		crl::on_main(controller.get(), [=] {
			ShowTopMessagesWindow(controller, peer, root);
		});
	});
	_close->setClickedCallback([=] {
		const auto controller = _controller;
		crl::on_main(controller.get(), [=] {
			Bars().remove(controller);
		});
	});

	sizeValue() | rpl::on_next([=] { relayout(); }, lifetime());

	paintRequest() | rpl::on_next([=] {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(st::shadowFg);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved({ 0.5, 0.5, 0.5, 0.5 }),
			radius,
			radius);
	}, lifetime());

	resize(st::boxWideWidth, st::historyComposeButton.height);
	updateText();
	updatePlacement();
	_placeTimer.callEach(250);

	// Up / Down step through results while the bar is showing.
	qApp->installEventFilter(this);
}

void ResultsBar::relayout() {
	const auto h = height();
	auto right = width() - st::normalFont->spacew;
	_close->moveToLeft(right - _close->width(), (h - _close->height()) / 2);
	right -= _close->width();
	_next->moveToLeft(right - _next->width(), (h - _next->height()) / 2);
	right -= _next->width();
	_previous->moveToLeft(
		right - _previous->width(),
		(h - _previous->height()) / 2);
	right -= _previous->width() + st::normalFont->spacew * 2;
	_list->moveToLeft(right - _list->width(), (h - _list->height()) / 2);
	right -= _list->width() + st::normalFont->spacew * 3;

	const auto left = st::normalFont->spacew * 4;
	const auto top = (h - _title->height() - _counter->height()) / 2;
	_title->resizeToWidth(std::max(right - left, 1));
	_title->moveToLeft(left, top);
	_counter->moveToLeft(left, top + _title->height());
}

bool ResultsBar::arrowsAllowed() const {
	if (isHidden() || QApplication::activeWindow() != window()) {
		return false;
	}
	// Keep the arrows for typing: only take them over an empty message box.
	const auto focus = QApplication::focusWidget();
	if (const auto edit = qobject_cast<QTextEdit*>(focus)) {
		return edit->document()->isEmpty();
	} else if (const auto line = qobject_cast<QLineEdit*>(focus)) {
		return line->text().isEmpty();
	}
	return true;
}

bool ResultsBar::eventFilter(QObject *o, QEvent *e) {
	if (e->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(e);
		const auto up = (key->key() == Qt::Key_Up);
		const auto down = (key->key() == Qt::Key_Down);
		const auto mods = key->modifiers() & ~Qt::KeypadModifier;
		if ((up || down) && mods == Qt::NoModifier && arrowsAllowed()) {
			const auto target = _index + (up ? -1 : 1);
			if (target >= 0 && target < int(_items.size())) {
				go(target);
			}
			return true;
		}
	}
	return RpWidget::eventFilter(o, e);
}

bool ResultsBar::inOurChat() const {
	const auto key = _controller->activeChatCurrent();
	if (key.peer() != _peer) {
		return false;
	} else if (_topicRootId) {
		const auto topic = key.topic();
		return topic && topic->rootId() == _topicRootId;
	}
	return true;
}

void ResultsBar::updatePlacement() {
	const auto parent = parentWidget();
	const auto area = inOurChat()
		? _controller->content()->ayuChatAreaWidget()
		: nullptr;
	auto scroll = (QAbstractScrollArea*)nullptr;
	if (area) {
		for (const auto child : area->findChildren<QAbstractScrollArea*>(
				Qt::FindDirectChildrenOnly)) {
			if (child->isVisible()
				&& (!scroll || child->height() > scroll->height())) {
				scroll = child;
			}
		}
	}
	if (!scroll || !parent) {
		hide();
		return;
	}
	const auto skip = st::historyComposeButton.height / 3;
	const auto origin = scroll->mapTo(parent, QPoint());
	const auto width = std::min(
		scroll->width() - 2 * skip,
		st::boxWideWidth + st::boxWideWidth / 3);
	const auto left = origin.x() + (scroll->width() - width) / 2;
	const auto top = origin.y() + scroll->height() - height() - skip;
	if (this->width() != width) {
		resize(width, height());
	}
	move(left, top);
	if (isHidden()) {
		show();
	}
	if (!parent->children().isEmpty() && parent->children().back() != this) {
		raise();
	}
}

void ResultsBar::updateText() {
	const auto total = int(_items.size());
	const auto &item = _items[_index];
	_title->setText(u"Top messages by "_q + SortName(_sort));
	_counter->setText(FormatNumber(_index + 1)
		+ u" of "_q
		+ FormatNumber(total)
		+ QString::fromUtf8("  \xC2\xB7  ")
		+ FormatNumber(item.metric)
		+ u" "_q
		+ SortName(_sort));
	_counter->resizeToNaturalWidth(width());

	const auto prevDisabled = (_index <= 0);
	const auto nextDisabled = (_index + 1 >= total);
	_previous->setAttribute(Qt::WA_TransparentForMouseEvents, prevDisabled);
	_next->setAttribute(Qt::WA_TransparentForMouseEvents, nextDisabled);
	_previous->setIconOverride(prevDisabled
		? &st::calendarNextDisabled
		: nullptr);
	_next->setIconOverride(nextDisabled
		? &st::calendarPreviousDisabled
		: nullptr);
	relayout();
}

void ResultsBar::go(int index) {
	if (_items.empty()) {
		return;
	}
	_index = std::clamp(index, 0, int(_items.size()) - 1);
	updateText();
	const auto &item = _items[_index];
	const auto controller = _controller;
	const auto peer = _peer;
	const auto id = item.id;
	const auto root = item.topicRoot;
	crl::on_main(controller.get(), [=] {
		JumpToResult(controller, peer, id, root);
	});
}

void ShowResultsBar(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		SortBy sort,
		std::vector<NavItem> items,
		int index) {
	const auto main = controller->content();
	if (items.empty()) {
		return;
	}
	auto &bars = Bars();
	if (bars.find(controller) == end(bars)) {
		controller->lifetime().add([=] {
			Bars().remove(controller);
		});
	}
	auto bar = base::make_unique_q<ResultsBar>(
		main,
		controller,
		peer,
		topicRootId,
		sort,
		std::move(items),
		index);
	const auto raw = bar.get();
	bars[controller] = std::move(bar);
	raw->go(index);
}

void TopMessagesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>();
	const auto key = CacheKey(peer->id, topicRootId);
	const auto forum = peer->isForum();
	const auto path = CachePath(session, key);
	state->results = CachedResults(key);
	if (!state->results) {
		state->results = LoadResults(path);
		if (state->results) {
			RememberResults(key, state->results);
		} else {
			state->results = std::make_shared<Results>();
		}
	}
	state->save = [=] {
		if (base::take(state->dirty) && state->results->hasScan) {
			SaveResults(path, *state->results);
		}
	};
	const auto initial = state->results;

	auto boxTitle = u"Top messages: "_q + peer->name();
	if (topicRootId) {
		if (const auto topic = peer->forumTopicFor(topicRootId)) {
			boxTitle = u"Top messages: "_q + topic->title();
		}
	}
	box->setTitle(rpl::single(boxTitle));
	box->setWidth(st::boxWideWidth);

	const auto content = box->verticalLayout();

	const auto tabs = content->add(
		object_ptr<Ui::SettingsSlider>(content, st::settingsSlider),
		st::settingsBigScalePadding);
	for (const auto &title : {
			u"Reactions"_q,
			u"Replies"_q,
			u"Forwards"_q,
			u"Views"_q }) {
		tabs->addSection(title);
	}
	tabs->setActiveSectionFast(int(initial->sort));

	Ui::AddSkip(content);
	const auto mediaOnly = content->add(object_ptr<Ui::SettingsButton>(
		content,
		rpl::single(u"Media only"_q),
		st::settingsButtonNoIcon));
	mediaOnly->toggleOn(rpl::single(initial->mediaOnly));

	const auto range = box->lifetime().make_state<
		rpl::variable<std::pair<int, int>>>(
			std::make_pair(initial->skip, initial->limit));

	const auto addSlider = [&](
			const QString &label,
			auto values,
			int current,
			Fn<QString(int)> text,
			Fn<void(int)> changed) {
		const auto valueText = box->lifetime().make_state<
			rpl::event_stream<QString>>();
		Settings::AddButtonWithLabel(
			content,
			rpl::single(label),
			valueText->events_starting_with(text(current)),
			st::autoDownloadLimitButton
		)->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto slider = content->add(
			object_ptr<Ui::MediaSlider>(content, st::autoDownloadLimitSlider),
			st::autoDownloadLimitPadding);
		slider->resize(st::autoDownloadLimitSlider.seekSize);
		slider->setPseudoDiscrete(
			int(values.size()),
			[=](int index) { return values[index]; },
			current,
			[=](int value) {
				valueText->fire(text(value));
				changed(value);
			});
	};
	addSlider(
		u"Start from"_q,
		kSkips,
		initial->skip,
		SkipText,
		[=](int value) {
			*range = std::make_pair(value, range->current().second);
		});
	addSlider(
		u"Messages to scan"_q,
		kLimits,
		initial->limit,
		LimitText,
		[=](int value) {
			*range = std::make_pair(range->current().first, value);
		});

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			range->value() | rpl::map([](std::pair<int, int> value) {
				return RangeText(value.first, value.second);
			}),
			st::boxDividerLabel),
		st::boxRowPadding);
	Ui::AddSkip(content);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->status.value(),
			st::boxDividerLabel),
		st::boxRowPadding);
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);

	const auto results = content->add(
		object_ptr<Ui::VerticalLayout>(content));

	// Opens the inline "X of Y" bar in the chat at this result,
	// and tucks the list window away (the bar's "List" brings it back).
	const auto browse = [=](
			std::vector<NavItem> items,
			int index,
			SortBy sort) {
		controller->window().activate();
		if (const auto window = box->window()) {
			window->hide();
		}
		ShowResultsBar(
			controller,
			peer,
			topicRootId,
			sort,
			std::move(items),
			index);
	};

	const auto refresh = &state->refresh;
	const auto showPage = [=](int page) {
		crl::on_main(box, [=] {
			state->page = page;
			state->scrollToTop = true;
			(*refresh)();
		});
	};
	*refresh = [=] {
		results->clear();
		const auto data = state->results;
		auto sorted = std::vector<const Entry*>();
		sorted.reserve(data->entries.size());
		for (const auto &entry : data->entries) {
			if ((!data->mediaOnly || entry.media)
				&& Metric(entry, data->sort) > 0) {
				sorted.push_back(&entry);
			}
		}
		const auto sort = data->sort;
		ranges::sort(sorted, [=](const Entry *a, const Entry *b) {
			const auto ma = Metric(*a, sort);
			const auto mb = Metric(*b, sort);
			return (ma != mb) ? (ma > mb) : (a->date > b->date);
		});
		const auto total = int(sorted.size());
		auto navItems = std::make_shared<std::vector<NavItem>>();
		navItems->reserve(sorted.size());
		for (const auto entry : sorted) {
			navItems->push_back({
				entry->id,
				entry->topicRoot,
				Metric(*entry, sort),
			});
		}
		const auto pages = std::max((total + kShownLimit - 1) / kShownLimit, 1);
		state->page = std::clamp(state->page, 0, pages - 1);
		const auto first = state->page * kShownLimit;
		const auto till = std::min(first + kShownLimit, total);
		const auto header = (total > 0)
			? results->add(
				object_ptr<Ui::FlatLabel>(
					results,
					rpl::single(u"Showing "_q
						+ FormatNumber(first + 1)
						+ QString::fromUtf8("\xE2\x80\x93")
						+ FormatNumber(till)
						+ u" of "_q
						+ FormatNumber(total)
						+ u" results (page "_q
						+ FormatNumber(state->page + 1)
						+ u" of "_q
						+ FormatNumber(pages)
						+ u")."_q),
					st::boxDividerLabel),
				st::boxRowPadding)
			: nullptr;
		if (total > 0) {
			Settings::AddButtonWithLabel(
				results,
				rpl::single(u"Browse results in chat"_q),
				rpl::single(u"from #1"_q),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				crl::on_main(box, [=] { browse(*navItems, 0, sort); });
			});
			const auto pageFirst = first;
			const auto pageTill = till;
			Settings::AddButtonWithLabel(
				results,
				rpl::single(u"Open this page as a chat"_q),
				rpl::single(u"#"_q
					+ FormatNumber(first + 1)
					+ QString::fromUtf8("\xE2\x80\x93")
					+ FormatNumber(till)),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				crl::on_main(box, [=] {
					auto ids = std::vector<FullMsgId>();
					ids.reserve(pageTill - pageFirst);
					for (auto i = pageFirst; i < pageTill
						&& i < int(navItems->size()); ++i) {
						ids.push_back(FullMsgId(peer->id, (*navItems)[i].id));
					}
					if (ids.empty()) {
						return;
					}
					controller->window().activate();
					if (const auto window = box->window()) {
						window->hide();
					}
					controller->showSection(
						std::make_shared<TopMessagesView::SectionMemento>(
							peer,
							std::move(ids),
							u"Top by "_q
								+ SortName(sort)
								+ u" #"_q
								+ FormatNumber(pageFirst + 1)
								+ QString::fromUtf8("\xE2\x80\x93")
								+ FormatNumber(pageTill)));
				});
			});
		}
		for (auto index = first; index != till; ++index) {
			const auto entry = sorted[index];
			const auto from = entry->from
				? session->data().peerLoaded(entry->from)
				: nullptr;
			const auto name = from ? from->shortName() : QString();
			const auto preview = !entry->text.isEmpty()
				? entry->text
				: entry->media
				? u"Media"_q
				: u"Message"_q;
			const auto text = u"#"_q
				+ FormatNumber(index + 1)
				+ u"  "_q
				+ (name.isEmpty() ? preview : (name + u": "_q + preview));
			const auto date = QLocale().toString(
				base::unixtime::parse(entry->date).date(),
				QLocale::ShortFormat);
			const auto label = FormatNumber(Metric(*entry, sort))
				+ QString::fromUtf8(" \xC2\xB7 ")
				+ date;
			Settings::AddButtonWithLabel(
				results,
				rpl::single(text),
				rpl::single(label),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				crl::on_main(box, [=] { browse(*navItems, index, sort); });
			});
		}
		if (pages > 1) {
			Ui::AddSkip(results);
			const auto page = state->page;
			if (page + 1 < pages) {
				Settings::AddButtonWithLabel(
					results,
					rpl::single(u"Next page"_q),
					rpl::single(FormatNumber(page + 2)
						+ u" of "_q
						+ FormatNumber(pages)),
					st::settingsButtonNoIcon
				)->addClickHandler([=] { showPage(page + 1); });
			}
			if (page > 0) {
				Settings::AddButtonWithLabel(
					results,
					rpl::single(u"Previous page"_q),
					rpl::single(FormatNumber(page)
						+ u" of "_q
						+ FormatNumber(pages)),
					st::settingsButtonNoIcon
				)->addClickHandler([=] { showPage(page - 1); });
				Settings::AddButtonWithLabel(
					results,
					rpl::single(u"First page"_q),
					rpl::single(QString()),
					st::settingsButtonNoIcon
				)->addClickHandler([=] { showPage(0); });
			}
		}
		if (sorted.empty() && !data->entries.empty()) {
			results->add(
				object_ptr<Ui::FlatLabel>(
					results,
					rpl::single(u"Nothing matches yet."_q),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
		results->resizeToWidth(content->width());
		if (header && state->scrollToTop) {
			state->scrollToTop = false;
			box->scrollToWidget(header);
		}
	};

	const auto updateStatus = [=] {
		const auto data = state->results;
		const auto of = data->limit
			? (u" of "_q + FormatNumber(data->limit))
			: QString();
		state->status = (state->scanning ? u"Scanning: "_q : u"Scanned "_q)
			+ FormatNumber(data->scanned)
			+ of
			+ u" messages"_q
			+ (data->skip
				? (u" (after skipping "_q + FormatNumber(data->skip) + u")"_q)
				: QString())
			+ u"."_q
			+ ((!state->scanning && data->reachedStart)
				? u" Reached the start."_q
				: QString());
	};

	const auto finish = [=](const QString &suffix) {
		state->scanning = false;
		state->timer.cancel();
		if (state->requestId) {
			session->api().request(base::take(state->requestId)).cancel();
		}
		state->buttonText = u"Scan"_q;
		state->dirty = true;
		state->save();
		updateStatus();
		if (!suffix.isEmpty()) {
			state->status = state->status.current() + u" "_q + suffix;
		}
		(*refresh)();
	};

	const auto done = [=](const MTPmessages_Messages &result) {
		state->requestId = 0;
		if (!state->scanning) {
			return;
		}
		const auto data = state->results;
		auto count = 0;
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &list) {
			session->data().processUsers(list.vusers());
			session->data().processChats(list.vchats());
			const auto &messages = list.vmessages().v;
			count = int(messages.size());
			ParseMessages(messages, data.get(), forum, topicRootId);
		});
		data->scanned += count;
		state->sinceRefresh += count;
		state->dirty = true;
		if (!count) {
			data->reachedStart = true;
			finish(QString());
			return;
		} else if (data->limit && data->scanned >= data->limit) {
			finish(QString());
			return;
		}
		updateStatus();
		if (state->sinceRefresh >= kRefreshEvery) {
			state->sinceRefresh = 0;
			(*refresh)();
		}
		state->timer.callOnce(kRequestDelay);
	};

	const auto fail = [=](const MTP::Error &error) {
		state->requestId = 0;
		if (!state->scanning) {
			return;
		}
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = type.mid(11).toInt();
			state->status = u"Telegram asked to slow down, waiting "_q
				+ QString::number(seconds)
				+ u"s..."_q;
			state->timer.callOnce(seconds * crl::time(1000)
				+ kFloodExtraDelay);
			return;
		}
		finish(u"Stopped: "_q + type);
	};

	state->timer.setCallback([=] {
		if (state->requestNext) {
			state->requestNext();
		}
	});
	state->requestNext = [=] {
		if (!state->scanning) {
			return;
		}
		const auto data = state->results;
		auto limit = kPerRequest;
		if (data->limit) {
			limit = std::min(limit, data->limit - data->scanned);
			if (limit <= 0) {
				finish(QString());
				return;
			}
		}
		const auto offsetId = int(data->offsetId.bare);
		const auto addOffset = offsetId ? 0 : data->skip;
		if (topicRootId) {
			state->requestId = session->api().request(MTPmessages_GetReplies(
				peer->input(),
				MTP_int(int(topicRootId.bare)),
				MTP_int(offsetId),
				MTP_int(0), // offset_date
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(crl::guard(box, done)).fail(crl::guard(box, fail)
			).handleFloodErrors().send();
		} else {
			state->requestId = session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(offsetId),
				MTP_int(0), // offset_date
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done(crl::guard(box, done)).fail(crl::guard(box, fail)
			).handleFloodErrors().send();
		}
	};

	const auto start = [=] {
		if (state->scanning) {
			finish(u"Stopped."_q);
			return;
		}
		const auto previous = state->results;
		const auto fresh = std::make_shared<Results>();
		fresh->sort = previous->sort;
		fresh->mediaOnly = previous->mediaOnly;
		fresh->skip = range->current().first;
		fresh->limit = range->current().second;
		fresh->hasScan = true;
		state->results = fresh;
		RememberResults(key, fresh);
		state->sinceRefresh = 0;
		state->page = 0;
		state->scanning = true;
		state->buttonText = u"Stop"_q;
		updateStatus();
		(*refresh)();
		state->requestNext();
	};

	tabs->sectionActivated(
	) | rpl::on_next([=](int index) {
		state->results->sort = SortBy(index);
		state->dirty = true;
		state->page = 0;
		(*refresh)();
	}, tabs->lifetime());

	mediaOnly->toggledChanges(
	) | rpl::on_next([=](bool value) {
		state->results->mediaOnly = value;
		state->dirty = true;
		state->page = 0;
		(*refresh)();
	}, mediaOnly->lifetime());

	if (initial->hasScan) {
		updateStatus();
		state->status = state->status.current()
			+ u" Showing your last scan, press Scan to rescan."_q;
		(*refresh)();
	} else {
		state->status = u"Pick a range and what to sort by, then press Scan."_q;
	}

	box->addButton(state->buttonText.value(), start);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void ShowTopMessagesWindow(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	const auto key = CacheKey(peer->id, topicRootId);
	auto &windows = Windows();
	auto i = windows.find(controller);
	if (i != end(windows)) {
		if (const auto panel = i->second.stack->currentPanel()) {
			if (i->second.key == key) {
				panel->show();
				panel->raise();
				panel->activateWindow();
				return;
			}
		}
	} else {
		const auto stack = controller->lifetime().make_state<
			Ui::StandaloneLayerStack>();
		i = windows.emplace(controller, OpenWindow{ stack, key }).first;
		controller->lifetime().add([=] {
			Windows().remove(controller);
		});
	}
	i->second.key = key;
	const auto stack = i->second.stack;
	stack->setAnchor(controller->widget()->geometry(), std::nullopt, {});
	stack->showBox(
		Box(TopMessagesBox, controller, peer, topicRootId),
		Ui::LayerOption::CloseOther,
		anim::type::normal);
	if (const auto panel = stack->currentPanel()) {
		// Keep the list above the main window while jumping to results.
		panel->setWindowFlag(Qt::WindowStaysOnTopHint, true);
		panel->show();
		panel->raise();
		panel->activateWindow();
	}
}
