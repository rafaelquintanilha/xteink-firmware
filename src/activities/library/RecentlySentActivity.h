#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// xteink: "Recently sent" on Home. The files the server delivered, newest first
// (by server item id, so it needs no clock), with the folder they landed in;
// Confirm opens one. Read once on entry from the delivered-file manifest.
class RecentlySentActivity final : public UiListActivity {
 public:
  RecentlySentActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("RecentlySent", renderer, input) {}

  void onEnter() override;

 private:
  static constexpr size_t MAX_ROWS = 20;

  struct Row {
    std::string path;
    std::string name;    // "Title - Author"
    std::string folder;  // "Articles", "Books", ...
  };
  std::vector<Row> entries;
  // ListItem points into `entries`; sized once in onEnter.
  std::vector<freeink::ui::ListItem> items;

  int listCount() const override { return entries.empty() ? 1 : static_cast<int>(entries.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
};
