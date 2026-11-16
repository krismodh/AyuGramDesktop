// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once


class History;
class HistoryItem;

namespace HistoryView {
class ElementDelegate;
class Element;
} // namespace HistoryView

namespace TopMessagesView {

class OwnedItem;

// Holds our own view of a real message (the message itself is not owned).
class OwnedItem
{
public:
	OwnedItem(std::nullptr_t = nullptr);
	OwnedItem(
		not_null<HistoryView::ElementDelegate*> delegate,
		not_null<HistoryItem*> data);
	OwnedItem(const OwnedItem &other) = delete;
	OwnedItem &operator=(const OwnedItem &other) = delete;
	OwnedItem(OwnedItem &&other);
	OwnedItem &operator=(OwnedItem &&other);
	~OwnedItem();

	[[nodiscard]] HistoryView::Element *get() const {
		return _view.get();
	}

	[[nodiscard]] HistoryView::Element *operator->() const {
		return get();
	}

	[[nodiscard]] operator HistoryView::Element*() const {
		return get();
	}

	void refreshView(not_null<HistoryView::ElementDelegate*> delegate);
	void clearView();

private:
	HistoryItem *_data = nullptr;
	std::unique_ptr<HistoryView::Element> _view;

};

} // namespace TopMessagesView
