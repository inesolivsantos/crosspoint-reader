#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LibraryBookMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    MARK_AS_READ,
    CLEAR_PROGRESS,
    DELETE_BOOK,
    CANCEL
  };

  explicit LibraryBookMenuActivity(
      GfxRenderer& renderer,
      MappedInputManager& mappedInput,
      const std::string& title
  );

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  const std::vector<MenuItem> menuItems = {
      {MenuAction::MARK_AS_READ, StrId::STR_MARK_AS_READ},
      {MenuAction::CLEAR_PROGRESS, StrId::STR_CLEAR_PROGRESS},
      {MenuAction::DELETE_BOOK, StrId::STR_DELETE},
      {MenuAction::CANCEL, StrId::STR_CANCEL},
  };

  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  std::string title;
};