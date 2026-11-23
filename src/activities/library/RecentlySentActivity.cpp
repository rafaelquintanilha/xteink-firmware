#include "RecentlySentActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "xteink/XteinkSync.h"

namespace fui = freeink::ui;

void RecentlySentActivity::onEnter() {
  entries.clear();
  const auto delivered = xteink::sync::recentDeliveries(MAX_ROWS);
  entries.reserve(delivered.size());
  for (const auto& f : delivered) {
    Row row;
    row.path = f.path;
    row.name = xteink::sync::deliveredDisplayName(f.path);
    // "/Articles/x.epub" -> "Articles"
    const size_t end = f.path.find_last_of('/');
    const size_t start = end == std::string::npos || end == 0 ? std::string::npos : f.path.find_last_of('/', end - 1);
    if (start != std::string::npos) row.folder = f.path.substr(start + 1, end - start - 1);
    entries.push_back(std::move(row));
  }
  items.assign(static_cast<size_t>(listCount()), fui::ListItem{});
  LOG_INF("RECENT", "%u delivered files listed", static_cast<unsigned>(entries.size()));
  UiListActivity::onEnter();
}

const char* RecentlySentActivity::headerTitle() const { return tr(STR_XTEINK_RECENT); }

void RecentlySentActivity::activateIndex(const int index) {
  if (entries.empty() || index < 0 || index >= static_cast<int>(entries.size())) return;
  app.clearTapFlash();
  onSelectBook(entries[static_cast<size_t>(index)].path);
}

void RecentlySentActivity::onBackButton() { activityManager.goHome(HomeMenuItem::XTEINK_RECENT); }

void RecentlySentActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (entries.empty()) {
    items[0].label = tr(STR_XTEINK_RECENT_EMPTY);
    items[0].enabled = false;
  } else {
    for (size_t i = 0; i < entries.size(); ++i) {
      items[i].label = entries[i].name.c_str();
      items[i].subtitle = entries[i].folder.empty() ? nullptr : entries[i].folder.c_str();
      items[i].actionValue = static_cast<int16_t>(i);
    }
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}
