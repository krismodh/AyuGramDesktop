// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/top_messages_box.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QLocale>

namespace {

constexpr auto kPerRequest = 100;
constexpr auto kRequestDelay = crl::time(350);
constexpr auto kFloodExtraDelay = crl::time(500);
constexpr auto kShownLimit = 100;
constexpr auto kPreviewLength = 90;
constexpr auto kRefreshEvery = 1000;
constexpr auto kLimits = std::array{
	500, 1000, 2000, 5000, 10000, 25000, 50000, 100000, 0,
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
	TimeId date = 0;
	PeerId from = 0;
	QString text;
	int reactions = 0;
	int replies = 0;
	int forwards = 0;
	int views = 0;
	bool media = false;
};

struct State {
	std::vector<Entry> entries;
	SortBy sort = SortBy::Reactions;
	bool mediaOnly = false;
	int limit = kLimits[kDefaultLimitIndex];
	MsgId offsetId = 0;
	int scanned = 0;
	int sinceRefresh = 0;
	bool scanning = false;
	mtpRequestId requestId = 0;
	base::Timer timer;
	Fn<void()> requestNext;
	rpl::variable<QString> status;
	rpl::variable<QString> buttonText = u"Scan"_q;
};

[[nodiscard]] QString FormatNumber(int value) {
	return QLocale().toString(value);
}

[[nodiscard]] QString LimitText(int limit) {
	return limit
		? (u"Last "_q + FormatNumber(limit) + u" messages"_q)
		: u"Whole chat"_q;
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

void ParseMessages(
		const QVector<MTPMessage> &list,
		not_null<State*> state) {
	for (const auto &message : list) {
		message.match([&](const MTPDmessage &data) {
			auto entry = Entry();
			entry.id = MsgId(data.vid().v);
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
			state->entries.push_back(std::move(entry));
			state->offsetId = MsgId(data.vid().v);
		}, [&](const MTPDmessageService &data) {
			state->offsetId = MsgId(data.vid().v);
		}, [&](const MTPDmessageEmpty &data) {
			state->offsetId = MsgId(data.vid().v);
		});
	}
}

} // namespace

void TopMessagesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &controller->session();
	const auto state = box->lifetime().make_state<State>();

	box->setTitle(rpl::single(u"Top messages"_q));
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
	tabs->setActiveSectionFast(0);

	Ui::AddSkip(content);
	const auto mediaOnly = content->add(object_ptr<Ui::SettingsButton>(
		content,
		rpl::single(u"Media only"_q),
		st::settingsButtonNoIcon));
	mediaOnly->toggleOn(rpl::single(false));

	const auto limitText = box->lifetime().make_state<
		rpl::event_stream<QString>>();
	Settings::AddButtonWithLabel(
		content,
		rpl::single(u"Messages to scan"_q),
		limitText->events_starting_with(LimitText(state->limit)),
		st::autoDownloadLimitButton
	)->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto slider = content->add(
		object_ptr<Ui::MediaSlider>(content, st::autoDownloadLimitSlider),
		st::autoDownloadLimitPadding);
	slider->resize(st::autoDownloadLimitSlider.seekSize);
	slider->setPseudoDiscrete(
		int(kLimits.size()),
		[](int index) { return kLimits[index]; },
		state->limit,
		[=](int value) {
			state->limit = value;
			limitText->fire(LimitText(value));
		});

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

	const auto refresh = [=] {
		results->clear();
		auto sorted = std::vector<const Entry*>();
		sorted.reserve(state->entries.size());
		for (const auto &entry : state->entries) {
			if ((!state->mediaOnly || entry.media)
				&& Metric(entry, state->sort) > 0) {
				sorted.push_back(&entry);
			}
		}
		const auto sort = state->sort;
		ranges::sort(sorted, [=](const Entry *a, const Entry *b) {
			const auto ma = Metric(*a, sort);
			const auto mb = Metric(*b, sort);
			return (ma != mb) ? (ma > mb) : (a->date > b->date);
		});
		if (int(sorted.size()) > kShownLimit) {
			sorted.resize(kShownLimit);
		}
		for (const auto entry : sorted) {
			const auto from = entry->from
				? session->data().peerLoaded(entry->from)
				: nullptr;
			const auto name = from ? from->shortName() : QString();
			const auto preview = !entry->text.isEmpty()
				? entry->text
				: entry->media
				? u"Media"_q
				: u"Message"_q;
			const auto text = name.isEmpty()
				? preview
				: (name + u": "_q + preview);
			const auto date = QLocale().toString(
				base::unixtime::parse(entry->date).date(),
				QLocale::ShortFormat);
			const auto label = FormatNumber(Metric(*entry, sort))
				+ QString::fromUtf8(" \xC2\xB7 ")
				+ date;
			const auto id = entry->id;
			Settings::AddButtonWithLabel(
				results,
				rpl::single(text),
				rpl::single(label),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				const auto weak = base::make_weak(controller);
				box->closeBox();
				crl::on_main([=] {
					if (const auto strong = weak.get()) {
						strong->showPeerHistory(
							peer,
							Window::SectionShow::Way::Forward,
							id);
					}
				});
			});
		}
		if (sorted.empty() && !state->entries.empty()) {
			results->add(
				object_ptr<Ui::FlatLabel>(
					results,
					rpl::single(u"Nothing matches yet."_q),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
		results->resizeToWidth(content->width());
	};

	const auto updateStatus = [=] {
		const auto of = state->limit
			? (u" of "_q + FormatNumber(state->limit))
			: QString();
		state->status = (state->scanning ? u"Scanning: "_q : u"Scanned "_q)
			+ FormatNumber(state->scanned)
			+ of
			+ u" messages."_q;
	};

	const auto finish = [=](const QString &suffix) {
		state->scanning = false;
		state->timer.cancel();
		if (state->requestId) {
			session->api().request(base::take(state->requestId)).cancel();
		}
		state->buttonText = u"Scan"_q;
		updateStatus();
		if (!suffix.isEmpty()) {
			state->status = state->status.current() + u" "_q + suffix;
		}
		refresh();
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
		auto limit = kPerRequest;
		if (state->limit) {
			limit = std::min(limit, state->limit - state->scanned);
			if (limit <= 0) {
				finish(QString());
				return;
			}
		}
		state->requestId = session->api().request(MTPmessages_GetHistory(
			peer->input(),
			MTP_int(int(state->offsetId.bare)),
			MTP_int(0),
			MTP_int(0),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(crl::guard(box, [=](const MTPmessages_Messages &result) {
			state->requestId = 0;
			if (!state->scanning) {
				return;
			}
			auto count = 0;
			result.match([&](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				const auto &list = data.vmessages().v;
				count = int(list.size());
				ParseMessages(list, state);
			});
			state->scanned += count;
			state->sinceRefresh += count;
			if (!count || (state->limit && state->scanned >= state->limit)) {
				finish(count ? QString() : u"Reached the start."_q);
				return;
			}
			updateStatus();
			if (state->sinceRefresh >= kRefreshEvery) {
				state->sinceRefresh = 0;
				refresh();
			}
			state->timer.callOnce(kRequestDelay);
		})).fail(crl::guard(box, [=](const MTP::Error &error) {
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
		})).handleFloodErrors().send();
	};

	const auto start = [=] {
		if (state->scanning) {
			finish(u"Stopped."_q);
			return;
		}
		state->entries.clear();
		state->offsetId = 0;
		state->scanned = 0;
		state->sinceRefresh = 0;
		state->scanning = true;
		state->buttonText = u"Stop"_q;
		updateStatus();
		refresh();
		state->requestNext();
	};

	tabs->sectionActivated(
	) | rpl::on_next([=](int index) {
		state->sort = SortBy(index);
		refresh();
	}, tabs->lifetime());

	mediaOnly->toggledChanges(
	) | rpl::on_next([=](bool value) {
		state->mediaOnly = value;
		refresh();
	}, mediaOnly->lifetime());

	state->status = u"Pick what to sort by, then press Scan."_q;

	box->addButton(state->buttonText.value(), start);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}
