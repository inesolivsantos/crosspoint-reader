#include "LibraryBookMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

LibraryBookMenuActivity::LibraryBookMenuActivity(
    GfxRenderer& renderer,
    MappedInputManager& mappedInput,
    const std::string& title
)
    : Activity("LibraryBookMenu", renderer, mappedInput),
      title(title) {}

void LibraryBookMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void LibraryBookMenuActivity::onExit() {
  Activity::onExit();
}

void LibraryBookMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex =
        ButtonNavigator::nextIndex(
            selectedIndex,
            static_cast<int>(menuItems.size())
        );

    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex =
        ButtonNavigator::previousIndex(
            selectedIndex,
            static_cast<int>(menuItems.size())
        );

    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const MenuAction selectedAction =
        menuItems[selectedIndex].action;

    if (selectedAction == MenuAction::CANCEL) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    setResult(
        MenuResult{
            static_cast<int>(selectedAction),
            0,
            0
        }
    );

    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;

    setResult(std::move(result));
    finish();
  }
}

void LibraryBookMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const std::string truncatedTitle =
      renderer.truncatedText(
          UI_12_FONT_ID,
          title.c_str(),
          pageWidth - 40,
          EpdFontFamily::BOLD
      );

  renderer.drawCenteredText(
      UI_12_FONT_ID,
      20,
      truncatedTitle.c_str(),
      true,
      EpdFontFamily::BOLD
  );

  const int lineHeight = 36;
  const int menuHeight =
      static_cast<int>(menuItems.size()) * lineHeight;

  const int startY =
      (pageHeight - menuHeight) / 2;

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY =
        startY + static_cast<int>(i) * lineHeight;

    const bool isSelected =
        static_cast<int>(i) == selectedIndex;

    if (isSelected) {
      renderer.fillRect(
          10,
          displayY,
          pageWidth - 20,
          lineHeight,
          1
      );
    }

    renderer.drawText(
        UI_10_FONT_ID,
        30,
        displayY + 4,
        I18N.get(menuItems[i].labelId),
        !isSelected
    );
  }

  const auto labels =
      mappedInput.mapLabels(
          tr(STR_BACK),
          tr(STR_SELECT),
          tr(STR_DIR_UP),
          tr(STR_DIR_DOWN)
      );

  GUI.drawButtonHints(
      renderer,
      labels.btn1,
      labels.btn2,
      labels.btn3,
      labels.btn4
  );

  renderer.displayBuffer();
}