// The settings window's search and its collapsible sections (one mechanism hides both), and the pages'
// names by number. Was the first half of pc_settings_visuals.inl.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// ---- Search and collapsible sections ----
// One mechanism serves both. While "hidden", whatever a page draws goes nowhere: clipped to nothing,
// disabled (no clicks, no focus), and the cursor is put back afterwards, so it takes no space. A
// search hides every page's content and shows only the setting rows whose name matches (the real
// controls, working there); a closed section hides everything up to the next section header. The
// pages themselves stay as they are: the helpers below (and the settings_* row helpers) do this.
static char g_search_text[96] = "";
bool g_search = false;                        // a query is in the box this frame
static int g_search_page = -1;                // the page drawing now (during a search, every page draws)
static int g_search_hits = 0;
static int g_hidden = 0;                      // hiding depth (0: drawing normally)
static float g_shown_y = 0.0f;                // where the next shown row goes while hiding (cursor y)
static bool g_section_open = true;            // the current section draws
static int g_section_index = 0;               // sections met on this page
std::string g_goto_label;                     // "go to" a search result: this row on g_goto_page
int g_goto_page = -1;
static double g_goto_flash = -10.0;
static std::string g_goto_flash_label;
static const char* const kSearchPages[] = {"Video", "Audio", "Game", "Controls", "Overlays", "", "Gecko Codes"};
static const char* const kPageTitles[] = {"VIDEO", "AUDIO", "GAME", "CONTROLS", "OVERLAYS", "", "GECKO CODES"};
// A page number as shown: a removed or unknown page is Video.
int settings_page_id(int page) { return page >= 0 && page <= 6 && page != 5 ? page : 0; }
const char* settings_page_name(int page) { return kSearchPages[settings_page_id(page)]; }
const char* settings_page_title(int page) { return kPageTitles[settings_page_id(page)]; }

static void settings_hide_begin() {
  if (g_hidden++ == 0) {
    g_shown_y = ImGui::GetCursorPosY();
    ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
    ImGui::BeginDisabled();
  }
}
static void settings_hide_end() {
  if (g_hidden == 0) return;
  if (--g_hidden == 0) {
    ImGui::EndDisabled();
    ImGui::PopClipRect();
    ImGui::SetCursorPosY(g_shown_y);
  }
}
static std::string settings_lower(const char* s) {
  std::string out;
  for (; s && *s; ++s) {
    if (s[0] == '#' && s[1] == '#') break;   // an ImGui id suffix is not part of the name
    out += (char)std::tolower((unsigned char)*s);
  }
  return out;
}
// Every word of the query matches the name or the page's name, forgivingly (settings_search.h:
// parts of words, typos, abbreviations, other words for the same thing).
static bool settings_search_matches(const char* label) {
  if (!label || label[0] == '#') return false;
  const std::string name = std::string(label) + " " + (g_search_page >= 0 && g_search_page < 7 ? kSearchPages[g_search_page] : "");
  return settings_search::matches(g_search_text, name);
}

// A setting row is about to draw. Returns whether it shows: always outside a search and a closed
// section; in a search only when it matches, and then it is drawn in the shown list, with its page's
// name to the right to go there. Pair a true answer with settings_row_end().
SettingsRow settings_row_begin(const char* label) {
  SettingsRow r;
  r.label = label ? label : "";
  if (g_search) {
    r.shown = settings_search_matches(label);
    if (r.shown && g_hidden > 0) {
      // Show it: stop hiding for this row, at the shown list's end.
      ImGui::EndDisabled();
      ImGui::PopClipRect();
      ImGui::SetCursorPosY(g_shown_y);
      r.revealed = true;
      ++g_search_hits;
      // Where it lives: the page's name, a button that goes there.
      ImGui::PushID(label);
      ImGui::PushID(g_search_page);
      const char* page = g_search_page >= 0 && g_search_page < 7 ? kSearchPages[g_search_page] : "";
      ImGui::TextDisabled("%s", page);
      ImGui::SameLine();
      if (ImGui::SmallButton("Go to")) {
        g_goto_label = r.label;
        g_goto_page = g_search_page;
        g_goto_flash = ImGui::GetTime();   // the request's time until the row is found
        g_search_text[0] = 0;
      }
      ImGui::PopID();
      ImGui::PopID();
      ImGui::PushID(g_search_page);   // the same name on two pages stays two controls (popped in settings_row_end)
    }
    return r;
  }
  // Normal drawing: a row asked for by "Go to" scrolls into view and flashes.
  if (!g_goto_label.empty() && g_goto_label == r.label && g_goto_page == g_search_page) {
    ImGui::SetScrollHereY(0.3f);
    g_goto_flash = ImGui::GetTime();
    g_goto_flash_label = r.label;
    g_goto_label.clear();
  }
  return r;
}
void settings_row_end(const SettingsRow& r) {
  if (!r.revealed) {
    if (!g_search && !r.label.empty() && r.label == g_goto_flash_label && ImGui::GetTime() - g_goto_flash < 1.6) {
      const float a = (float)(1.0 - (ImGui::GetTime() - g_goto_flash) / 1.6);
      ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                          IM_COL32(255, 210, 90, (int)(255 * a)), 6.0f, 0, 2.5f);
    }
    return;
  }
  ImGui::PopID();
  ImGui::Spacing();
  g_shown_y = ImGui::GetCursorPosY();
  ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
  ImGui::BeginDisabled();
}

// The pages' own ImGui controls, searchable the same way (a search shows the matching ones).
static std::vector<SettingsRow> g_open_combos;
bool settings_begin_combo(const char* label, const char* preview, ImGuiComboFlags flags) {
  SettingsRow row = settings_row_begin(label);
  if (!row.shown) return false;
  if (!settings_widgets::BeginCombo(label, preview, flags)) { settings_row_end(row); return false; }
  g_open_combos.push_back(std::move(row));
  return true;
}
void settings_end_combo() {
  ImGui::EndCombo();
  if (g_open_combos.empty()) return;
  const SettingsRow row = std::move(g_open_combos.back());
  g_open_combos.pop_back();
  settings_row_end(row);
}
template <class Draw> static bool settings_searchable(const char* label, Draw draw) {
  const SettingsRow row = settings_row_begin(label);
  if (!row.shown) return false;
  const bool changed = draw();
  settings_row_end(row);
  return changed;
}
bool settings_slider_int(const char* label, int* v, int lo, int hi, const char* format, ImGuiSliderFlags flags) {
  return settings_searchable(label, [&] { return settings_widgets::SliderInt(label, v, lo, hi, format, flags); });
}
bool settings_color_edit3(const char* label, float col[3], ImGuiColorEditFlags flags) {
  return settings_searchable(label, [&] { return settings_widgets::ColorEdit3(label, col, flags); });
}

// A page starts (pc_settings.cpp: `if (settings_page_shown(state.active_tab, N))`). During a search every
// page draws, hidden; otherwise only the active one.
bool settings_page_shown(int active_tab, int page) {
  if (!g_search && active_tab != page) return false;
  // The page before ends: its last section closes.
  if (!g_search) settings_hide_end();
  g_search_page = page;
  g_section_index = 0;
  g_section_open = true;
  return true;
}

// A section header: a line that folds the settings under it, up to the next header. Every section
// starts open (owner, 0.9.210: settings should not hide); each one's state is remembered. During a search
// sections draw no header and every setting is searched.
void settings_section(const char* title) {
  const int index = g_section_index++;
  if (g_search) return;
  settings_hide_end();   // the section before ends
  ImGuiStorage* storage = ImGui::GetStateStorage();
  ImGui::PushID("section");
  ImGui::PushID(g_search_page);
  ImGui::PushID(title);
  const ImGuiID key = ImGui::GetID("open");
  bool open = storage->GetBool(key, true);
  (void)index;
  // "Go to" opens the section the row is in: until it is drawn, every section of that page opens.
  if (!g_goto_label.empty() && g_goto_page == g_search_page) open = true;
  ImGui::Spacing();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight() + 6.0f;
  if (ImGui::InvisibleButton("##header", ImVec2(width, height))) { open = !open; }
  storage->SetBool(key, open);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), hot ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10), 5.0f);
  const float mid = at.y + height * 0.5f, s = ImGui::GetFontSize() * 0.32f;
  const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
  if (open)
    draw->AddTriangleFilled(ImVec2(at.x + 8, mid - s * 0.6f), ImVec2(at.x + 8 + 2 * s, mid - s * 0.6f), ImVec2(at.x + 8 + s, mid + s * 0.8f), ink);
  else
    draw->AddTriangleFilled(ImVec2(at.x + 9, mid - s), ImVec2(at.x + 9, mid + s), ImVec2(at.x + 9 + 1.6f * s, mid), ink);
  draw->AddText(ImVec2(at.x + 16 + 2 * s, mid - ImGui::GetFontSize() * 0.5f), ink, title);
  ImGui::PopID();
  ImGui::PopID();
  ImGui::PopID();
  g_section_open = open;
  if (!open) settings_hide_begin();
}

// After the last page: close any hiding, and size the scroll to what was shown.
void settings_pages_end() {
  while (g_hidden > 0) settings_hide_end();
  if (g_search) {
    ImGui::SetCursorPosY(g_shown_y);
    if (g_search_hits == 0) ImGui::TextDisabled("No setting matches \"%s\".", g_search_text);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    w->DC.CursorMaxPos.y = ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(0, 0));
  }
}

// The search box at the top of every page. Typing starts a search over every page.
void settings_search_box() {
  // MELEE_TEST_SETTINGS_SEARCH=<words> (tests): the box starts with that query.
  static bool test_query_set = false;
  if (!test_query_set) {
    test_query_set = true;
    if (const char* q = host::test_env::getenv("MELEE_TEST_SETTINGS_SEARCH")) std::snprintf(g_search_text, sizeof g_search_text, "%s", q);
  }
  // Ctrl+F or "/" from anywhere in the settings puts the cursor in the box.
  const ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsAnyItemActive() &&
      ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) || ImGui::IsKeyPressed(ImGuiKey_Slash, false)))
    ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##settings_search", "Search all settings (Ctrl+F)", g_search_text, sizeof g_search_text);
  g_search = g_search_text[0] != 0;
  g_search_hits = 0;
  // A "Go to" whose row never drew (it sits in a closed list of its own) gives up after a second.
  if (!g_goto_label.empty() && ImGui::GetTime() - g_goto_flash > 1.0) g_goto_label.clear();
  g_hidden = 0;
  g_search_page = -1;
  ImGui::Spacing();
  if (g_search) settings_hide_begin();
}

}  // namespace gx
