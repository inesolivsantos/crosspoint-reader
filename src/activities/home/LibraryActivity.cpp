#include "LibraryActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "Bitmap.h"
#include "CrossPointSettings.h"
#include "Epub.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "Xtc.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void LibraryActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));

    std::string filenameFull{name};

    if (filenameFull.find(".crosspoint") != std::string::npos || filenameFull.find("XTCache") != std::string::npos ||
        filenameFull.find("update") != std::string::npos || filenameFull.find("Pushed Fonts") != std::string::npos ||
        filenameFull.find("Pushed Images") != std::string::npos) {
      continue;
    }
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void LibraryActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void LibraryActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;
  pageIndex = 0;

  // If Confirm was held while this activity opened, ignore its release.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());

  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);

    selectorIndex = findEntry(fileName);
    pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;
  } else {
    loadFiles();
  }

  requestUpdate();
}

void LibraryActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void LibraryActivity::loop() {
  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (!isDirectory) {
            clearFileMetadata(fullPath);
          }
          const bool deleted = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
          if (deleted) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              // Move selection to the new "last" item
              selectorIndex = files.size() - 1;
            }

            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        pageIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);
        pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    pageIndex = maxVisible > 0 ? selectorIndex / maxVisible : 0;
    requestUpdate();
  });
}

static std::string getFileName(std::string filename) {
  if (filename.empty()) {
    return "";
  }

  // Se vier com caminho completo, fica só com o nome do ficheiro/pasta
  const auto slashPos = filename.find_last_of('/');
  if (slashPos != std::string::npos && slashPos + 1 < filename.length()) {
    filename = filename.substr(slashPos + 1);
  }

  // Pasta
  if (!filename.empty() && filename.back() == '/') {
    filename.pop_back();
    return filename;
  }

  // Remove extensão
  const auto dotPos = filename.rfind('.');
  if (dotPos != std::string::npos) {
    filename = filename.substr(0, dotPos);
  }

  return filename;
}

static std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;

  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    const int columns = 2;
    const int rows = 2;

    const int tileWidth = pageWidth / columns;
    const int tileHeight = contentHeight / rows;

    maxVisible = columns * rows;

    const size_t startIndex = pageIndex * maxVisible;
    const size_t endIndex = std::min(files.size(), startIndex + maxVisible);

    for (size_t i = startIndex; i < endIndex; i++) {
      size_t localIndex = i - startIndex;

      int row = localIndex / columns;
      int col = localIndex % columns;

      int x = col * tileWidth;
      int y = contentTop + row * tileHeight;

      bool selected = (i == selectorIndex);

      Rect tileRect{x + 8, y + 4, tileWidth - 16, tileHeight - 8};

      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int titleAreaHeight = titleLineHeight * 2 + 8;

      const int coverWidth = tileRect.width - 18;
      const int coverHeight = tileRect.height - titleAreaHeight - 12;

      const int coverX = tileRect.x + (tileRect.width - coverWidth) / 2;
      const int coverY = tileRect.y + 2;
      const int titleY = coverY + coverHeight + 6;

      const int bookCoverWidth = coverHeight * 2 / 3;
      const int bookCoverX = coverX + (coverWidth - bookCoverWidth) / 2;

      const std::string& file = files[i];
      const std::string fallbackLabel = getFileName(file);
      bool isFolder = !file.empty() && file.back() == '/';

      std::string displayTitle = fallbackLabel;
      std::string displayAuthor = "";

      if (selected) {
        renderer.drawRect(bookCoverX - 2, coverY - 2, bookCoverWidth + 4, coverHeight + 4, 1);
        renderer.drawRect(bookCoverX, coverY, bookCoverWidth, coverHeight, 1);
      }

      bool renderedCover = false;

      if (isFolder) {
        renderer.drawRect(coverX, coverY, coverWidth, coverHeight, 1);

        const int folderW = 72;
        const int folderH = 48;
        const int tabW = 30;
        const int tabH = 12;

        const int folderX = coverX + (coverWidth - folderW) / 2;
        const int folderY = coverY + (coverHeight - folderH) / 2;

        renderer.drawRect(folderX + 6, folderY, tabW, tabH, 1);
        renderer.drawRect(folderX, folderY + tabH - 2, folderW, folderH - tabH + 2, 1);

        renderedCover = true;
      } else {
        FsFile fileObj;
        Bitmap bitmap(fileObj);

        std::string fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += file;

        if (FsHelpers::hasEpubExtension(file)) {
          std::string coverTemplatePath;

          const auto& recentBooks = RECENT_BOOKS.getBooks();

          for (const RecentBook& recentBook : recentBooks) {
            if (!recentBook.coverBmpPath.empty()) {
              const auto pos = recentBook.path.find_last_of('/');
              const std::string recentFileName =
                  (pos == std::string::npos) ? recentBook.path : recentBook.path.substr(pos + 1);

              if (recentFileName == file) {
                coverTemplatePath = recentBook.coverBmpPath;

                if (!recentBook.title.empty()) {
                  displayTitle = recentBook.title;
                }

                if (!recentBook.author.empty()) {
                  displayAuthor = recentBook.author;
                }

                break;
              }
            }
          }

          Epub epub(fullPath, "/.crosspoint");

          if (coverTemplatePath.empty() || displayAuthor.empty()) {
            if (epub.load(false, true)) {
              if (!epub.getTitle().empty()) {
                displayTitle = epub.getTitle();
              }

              if (!epub.getAuthor().empty()) {
                displayAuthor = epub.getAuthor();
              }

              if (coverTemplatePath.empty()) {
                coverTemplatePath = epub.getThumbBmpPath();
              }
            }
          }

          if (!coverTemplatePath.empty()) {
            const int thumbnailHeight = coverHeight;

            std::string coverBmpPath = UITheme::getCoverThumbPath(coverTemplatePath, thumbnailHeight);

            if (!Storage.exists(coverBmpPath.c_str())) {
              if (epub.load(false, true)) {
                epub.generateThumbBmp(thumbnailHeight);
              }
            }

            if (Storage.openFileForRead("LIB", coverBmpPath, fileObj)) {
              if (bitmap.parseHeaders() == BmpReaderError::Ok) {
                const int bitmapWidth = bitmap.getWidth();
                const int bitmapHeight = bitmap.getHeight();

                int renderedWidth = bitmapWidth;

                if (bitmapHeight > 0 && bitmapHeight != coverHeight) {
                  renderedWidth = static_cast<int>(static_cast<float>(bitmapWidth) * static_cast<float>(coverHeight) /
                                                   static_cast<float>(bitmapHeight));
                }

                renderedWidth = std::min(renderedWidth, coverWidth);

                const int renderedX = coverX + (coverWidth - renderedWidth) / 2;

                renderer.drawBitmap(bitmap, renderedX, coverY, renderedWidth, coverHeight);

                renderedCover = true;
              }
            }

            if (!renderedCover) {
              const int homeCoverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
              std::string homeCoverBmpPath = UITheme::getCoverThumbPath(coverTemplatePath, homeCoverHeight);

              FsFile fallbackFileObj;
              Bitmap fallbackBitmap(fallbackFileObj);

              if (Storage.openFileForRead("LIB", homeCoverBmpPath, fallbackFileObj)) {
                if (fallbackBitmap.parseHeaders() == BmpReaderError::Ok) {
                  const int fallbackWidth = fallbackBitmap.getWidth();
                  const int fallbackHeight = fallbackBitmap.getHeight();

                  int renderedFallbackWidth = fallbackWidth;

                  if (fallbackHeight > 0 && fallbackHeight != coverHeight) {
                    renderedFallbackWidth =
                        static_cast<int>(static_cast<float>(fallbackWidth) * static_cast<float>(coverHeight) /
                                         static_cast<float>(fallbackHeight));
                  }

                  renderedFallbackWidth = std::min(renderedFallbackWidth, coverWidth);

                  const int fallbackX = coverX + (coverWidth - renderedFallbackWidth) / 2;

                  renderer.drawBitmap(fallbackBitmap, fallbackX, coverY, renderedFallbackWidth, coverHeight);
                  renderedCover = true;
                }
              }
            }
          }
        }
      }

      if (!renderedCover) {
        renderer.drawRect(bookCoverX, coverY, bookCoverWidth, coverHeight, 1);
      }

      std::string titleLine1 = displayTitle;
      std::string titleLine2 = displayAuthor;

      const size_t maxLineChars = 22;

      if (titleLine1.length() > maxLineChars) {
        size_t breakPos = titleLine1.rfind(' ', maxLineChars);

        if (breakPos == std::string::npos || breakPos < 6) {
          breakPos = maxLineChars;
        }

        titleLine2 = titleLine1.substr(breakPos);
        titleLine1 = titleLine1.substr(0, breakPos);

        while (!titleLine2.empty() && titleLine2.front() == ' ') {
          titleLine2.erase(0, 1);
        }
      }

      renderer.drawText(SMALL_FONT_ID, tileRect.x + 4, titleY, titleLine1.c_str());

      if (!titleLine2.empty()) {
        renderer.drawText(SMALL_FONT_ID, tileRect.x + 4, titleY + titleLineHeight, titleLine2.c_str());
      }
    }
  }

  // Full path display
  const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
  const int separatorY = pathY - metrics.verticalSpacing / 2;

  renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);

  const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
  const char* pathStr = basepath.c_str();
  const char* pathDisplay = pathStr;
  char leftTruncBuf[256];

  if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
    const char ellipsis[] = "\xe2\x80\xa6";
    const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
    const int available = pathMaxWidth - ellipsisWidth;

    const char* p = pathStr;
    while (*p) {
      if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
      ++p;
      while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
    }

    snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
    pathDisplay = leftTruncBuf;
  }

  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);

  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';

  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t LibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
