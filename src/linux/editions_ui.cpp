// Linux album-edition manager and track comparison, a modeless overlay in the same style as the
// other Linux panels. All edits go through album_editions.cpp; playback keeps running behind it.
#include "album_editions.h"
#include "app.h"
#include "draw.h"
#include "libview.h"
#include "panels.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace lx {
namespace {

struct EditionsState {
    uint32_t group = 0;
    uint32_t selected = 0;
    uint32_t a = 0, b = 0;
    TrackId focus = kNone;
    bool compare = false;
    std::wstring edit;
    bool editFocus = false;
    size_t caret = 0;
    std::vector<uint32_t> editions;
    std::vector<EditionComparisonRow> rows;
    int row = -1;
    float scroll = 0;
};

EditionsState e;

Rect PanelRect(float w, float h) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    w = std::min((float)pw - S(16), w);
    h = std::min((float)ph - S(16), h);
    return {std::round((pw - w) / 2), std::round((ph - h) / 2), w, h};
}

const AlbumGroup* Group() { return FindAlbumGroupConst(e.group); }
const AlbumEdition* Edition(uint32_t id) {
    const AlbumGroup* group = Group();
    return group ? FindAlbumEdition(*group, id) : nullptr;
}

void Changed() {
    InvalidateCellCache();
    RebuildRows();
    g.libraryDirty = true;
    g.redraw = true;
}

void Refresh() {
    e.editions.clear();
    const AlbumGroup* group = Group();
    if (!group) {
        ClosePanel();
        return;
    }
    for (const auto& edition : group->editions) e.editions.push_back(edition.id);
    if (std::find(e.editions.begin(), e.editions.end(), e.selected) == e.editions.end())
        e.selected = PreferredEditionId(*group);
    if (const AlbumEdition* edition = Edition(e.selected)) {
        e.edit = edition->label.empty() ? EditionLabel(*edition) : FromUtf8(edition->label);
        e.caret = e.edit.size();
    }
    e.row = -1;
    for (size_t i = 0; i < e.editions.size(); ++i)
        if (e.editions[i] == e.selected) e.row = (int)i;
}

void ApplyRename() {
    const AlbumEdition* edition = Edition(e.selected);
    if (!edition) return;
    std::wstring label = e.edit;
    while (!label.empty() && iswspace(label.front())) label.erase(label.begin());
    while (!label.empty() && iswspace(label.back())) label.pop_back();
    if (label == EditionLabel(*edition)) label.clear();  // keep the automatic label if unchanged
    if (SetEditionLabel(e.group, e.selected, label)) {
        Refresh();
        Changed();
    }
}

void OpenCompare() {
    const AlbumGroup* group = Group();
    const AlbumEdition* selected = Edition(e.selected);
    if (!group || !selected || group->editions.size() < 2) return;
    e.compare = true;
    e.a = selected->id;
    e.b = PreferredEditionId(*group);
    if (e.b == e.a) {
        for (const auto& edition : group->editions)
            if (edition.id != e.a) { e.b = edition.id; break; }
    }
    e.focus = kNone;
    e.rows = CompareEditions(e.group, e.a, e.b);
    e.row = -1;
    for (size_t i = 0; i < e.rows.size(); ++i)
        if (e.rows[i].a != kInvalidTrack) { e.row = (int)i; break; }
    g.redraw = true;
}

void BackToManager() {
    e.compare = false;
    e.rows.clear();
    Refresh();
    g.redraw = true;
}

void ApplyCompare(bool confirm) {
    const AlbumGroup* group = Group();
    if (!group || e.row < 0 || e.row >= (int)e.rows.size()) return;
    const EditionComparisonRow& row = e.rows[(size_t)e.row];
    if (row.a == kInvalidTrack || row.b == kInvalidTrack) return;
    if (SetTrackMatch(e.group, row.a, row.b, confirm ? MatchStatus::Confirmed : MatchStatus::Rejected, row.relation)) {
        e.rows = CompareEditions(e.group, e.a, e.b);
        e.row = std::min(e.row, (int)e.rows.size() - 1);
        Changed();
    }
}

std::wstring TrackTitle(TrackId id) {
    if (id == kInvalidTrack || !g_lib.Contains(id)) return L"(missing)";
    std::wstring title = g_lib.Str(g_lib[id].title ? g_lib[id].title : g_lib[id].file);
    if (title.empty()) title = StemOf(g_lib.Path(id));
    return title;
}

std::wstring EditionName(uint32_t id) {
    const AlbumEdition* edition = Edition(id);
    return edition ? EditionLabel(*edition) : L"edition";
}

std::wstring CompareLine(const EditionComparisonRow& row) {
    if (row.a == kInvalidTrack) return L"      \x2022  " + TrackTitle(row.b) + L"  (only in " + EditionName(e.b) + L")";
    if (row.b == kInvalidTrack) return TrackTitle(row.a) + L"  (only in " + EditionName(e.a) + L")";
    const wchar_t* relation = L"unknown";
    switch (row.relation) {
        case TrackRelation::SameRecording: relation = L"same recording"; break;
        case TrackRelation::DifferentMaster: relation = L"different master"; break;
        case TrackRelation::AlternateMix: relation = L"alternate mix"; break;
        case TrackRelation::DifferentPerformance: relation = L"different performance"; break;
        default: break;
    }
    const wchar_t* status = row.status == MatchStatus::Confirmed ? L"confirmed" :
                            row.status == MatchStatus::Rejected ? L"rejected" : L"suggested";
    return TrackTitle(row.a) + L"  \x2194  " + TrackTitle(row.b) + L"  \x2022  " + relation + L", " + status;
}

Rect ManagerButton(int index) {
    Rect panel = PanelRect(S(780), S(570));
    return {panel.R() - S(274), panel.y + S(64) + index * S(34), S(250), S(28)};
}

Rect ManagerList() {
    Rect panel = PanelRect(S(780), S(570));
    return {panel.x + S(24), panel.y + S(64), panel.w - S(322), panel.h - S(174)};
}

Rect ManagerEdit() {
    Rect panel = PanelRect(S(780), S(570));
    return {panel.x + S(24), panel.B() - S(98), panel.w - S(322) - S(94), S(28)};
}

Rect ManagerRenameButton() {
    Rect edit = ManagerEdit();
    return {edit.R() + S(8), edit.y, S(86), edit.h};
}

Rect CompareButton(int index) {
    Rect panel = PanelRect(S(900), S(570));
    return {panel.x + S(24) + index * S(158), panel.B() - S(46), S(150), S(28)};
}

Rect CompareList() {
    Rect panel = PanelRect(S(900), S(570));
    return {panel.x + S(24), panel.y + S(64), panel.w - S(48), panel.h - S(164)};
}

void DrawManagerButtons(const Palette& p) {
    const char* labels[] = {"Set preferred", "Rename label", "Use this cover", "Clear cover", "Remove edition",
                            "Split group", "Compare editions", "Allow automatic", "Close"};
    for (int i = 0; i < 9; ++i) {
        bool enabled = true;
        const AlbumGroup* group = Group();
        if (i >= 3 && group) {
            if (i == 3) enabled = group->artTrack != kInvalidTrack;
            if (i == 4 || i == 5 || i == 6) enabled = group->editions.size() > 1;
            if (i == 7) enabled = group->manual;
        }
        DrawButton(ManagerButton(i), labels[i], p, i == 0, enabled);
    }
}

void DrawEditionRows(const Palette& p) {
    Rect list = ManagerList();
    Fill(list, p.bg);
    Frame(list, p.border);
    const AlbumGroup* group = Group();
    if (!group) return;
    uint32_t preferred = PreferredEditionId(*group);
    float rowH = S(30);
    int first = std::max(0, (int)(e.scroll / rowH));
    for (size_t i = (size_t)first; i < e.editions.size(); ++i) {
        float y = list.y + S(4) + (i * rowH) - e.scroll;
        if (y + rowH > list.B()) break;
        Rect row{list.x + S(1), y, list.w - S(2), rowH};
        const AlbumEdition* edition = Edition(e.editions[i]);
        if (!edition) continue;
        if ((int)i == e.row) Fill(row, p.select);
        else if (e.editions[i] == preferred) Fill(row, Mix(p.bg, p.accent, 12));
        std::wstring line = (edition->id == preferred ? L"\x2713  " : L"    ") + EditionLabel(*edition) + L"  \x2022  " +
                            std::to_wstring(edition->tracks.size()) + (edition->tracks.size() == 1 ? L" track" : L" tracks");
        DrawText(Font::Normal, Utf8(line), row.x + S(10), TextY(row), e.row == (int)i ? p.selectText : p.text,
                 row.w - S(16));
    }
}

void DrawManager(const Palette& p) {
    DrawFrame(p, S(780), S(570), "Album editions");
    DrawEditionRows(p);
    DrawManagerButtons(p);
    Rect edit = ManagerEdit();
    Fill(edit, p.surface2);
    Frame(edit, e.editFocus ? p.accent : p.border);
    DrawText(Font::Normal, Utf8(e.edit), edit.x + S(6), TextY(edit), p.text, edit.w - S(12));
    DrawButton(ManagerRenameButton(), "Rename label", p);
    Rect panel = PanelRect(S(780), S(570));
    DrawText(Font::Small,
             "Editions share one album tile. To add an album, select album tiles in the library and use "
             "Library > Album editions > Group selected albums as editions. Files and playlists are never changed.",
             panel.x + S(24), panel.B() - S(42), p.sub, panel.w - S(48));
}

void DrawCompare(const Palette& p) {
    DrawFrame(p, S(900), S(570), "Compare editions");
    Rect list = CompareList();
    Fill(list, p.bg);
    Frame(list, p.border);
    float rowH = S(26);
    for (size_t i = 0; i < e.rows.size(); ++i) {
        float y = list.y + S(4) + i * rowH;
        if (y + rowH > list.B()) break;
        Rect row{list.x + S(1), y, list.w - S(2), rowH};
        if ((int)i == e.row) Fill(row, p.select);
        DrawText(Font::Small, Utf8(CompareLine(e.rows[i])), row.x + S(8), TextY(row, Font::Small),
                 (int)i == e.row ? p.selectText : p.text, row.w - S(14));
    }
    const AlbumGroup* group = Group();
    std::wstring hint = group ? FromUtf8(group->title) + L"  \x2022  " + FromUtf8(group->artist) : L"";
    Rect panel = PanelRect(S(900), S(570));
    DrawText(Font::Small, Utf8(hint), panel.x + S(24), panel.y + S(42), p.sub, panel.w - S(48));
    DrawButton(CompareButton(0), "Confirm match", p, true);
    DrawButton(CompareButton(1), "Reject match", p);
    DrawButton(CompareButton(2), "Prefer second", p);
    DrawButton(CompareButton(3), "Back", p);
    DrawButton(CompareButton(4), "Close", p);
}

void SelectRow(float y) {
    Rect list = e.compare ? CompareList() : ManagerList();
    float rowH = e.compare ? S(26) : S(30);
    int row = (int)std::floor((y - list.y - S(4) + e.scroll) / rowH);
    if (row < 0) return;
    e.row = row;
    if (!e.compare && row < (int)e.editions.size()) {
        e.selected = e.editions[(size_t)row];
        const AlbumEdition* edition = Edition(e.selected);
        if (edition) {
            e.edit = edition->label.empty() ? EditionLabel(*edition) : FromUtf8(edition->label);
            e.caret = e.edit.size();
            e.editFocus = false;
        }
    }
}

void TakeArt() {
    const AlbumEdition* edition = Edition(e.selected);
    if (edition && !edition->tracks.empty() && SetAlbumGroupArt(e.group, edition->tracks.front())) {
        Refresh();
        Changed();
    }
}

}  // namespace

void EditionsOpenManager(uint32_t group) {
    e = EditionsState{};
    e.group = group;
    if (const AlbumGroup* found = Group()) e.selected = PreferredEditionId(*found);
    Refresh();
}

void EditionsOpenComparison(uint32_t group, TrackId focusTrack) {
    e = EditionsState{};
    e.group = group;
    const AlbumGroup* found = Group();
    if (!found) return;
    e.focus = focusTrack;
    if (const AlbumEdition* edition = AlbumEditionForTrack(focusTrack)) e.a = edition->id;
    if (!e.a) e.a = PreferredEditionId(*found);
    e.b = PreferredEditionId(*found);
    if (e.b == e.a)
        for (const auto& edition : found->editions)
            if (edition.id != e.a) { e.b = edition.id; break; }
    e.compare = true;
    e.rows = CompareEditions(group, e.a, e.b);
    e.row = -1;
    for (size_t i = 0; i < e.rows.size(); ++i)
        if (e.rows[i].a == focusTrack || e.rows[i].b == focusTrack) { e.row = (int)i; break; }
    if (e.row < 0 && !e.rows.empty()) e.row = 0;
}

void DrawEditionsPanel(const Palette& p) {
    if (!Group()) {
        ClosePanel();
        return;
    }
    if (e.compare) DrawCompare(p);
    else DrawManager(p);
}

void EditionsMouseDown(float x, float y, int button, int clicks) {
    if (button != SDL_BUTTON_LEFT) return;
    if (e.compare) {
        if (CompareList().In(x, y)) {
            SelectRow(y);
            if (clicks >= 2) ApplyCompare(true);
            g.redraw = true;
            return;
        }
        for (int i = 0; i < 5; ++i)
            if (CompareButton(i).In(x, y)) {
                if (i == 0) ApplyCompare(true);
                else if (i == 1) ApplyCompare(false);
                else if (i == 2) {
                    if (e.row >= 0 && e.row < (int)e.rows.size() && e.rows[(size_t)e.row].b != kInvalidTrack &&
                        SetPreferredTrackVersion(e.group, e.rows[(size_t)e.row].b))
                        Changed();
                } else if (i == 3) BackToManager();
                else ClosePanel();
                g.redraw = true;
                return;
            }
        return;
    }
    if (ManagerList().In(x, y)) {
        SelectRow(y);
        g.redraw = true;
        return;
    }
    if (ManagerEdit().In(x, y)) {
        e.editFocus = true;
        SDL_StartTextInput(g.window);
        g.redraw = true;
        return;
    }
    if (ManagerRenameButton().In(x, y)) {
        ApplyRename();
        return;
    }
    for (int i = 0; i < 9; ++i) {
        if (!ManagerButton(i).In(x, y)) continue;
        switch (i) {
            case 0:
                if (SetPreferredEdition(e.group, e.selected)) {
                    CanvasChooseEdition(e.group, e.selected);
                    Refresh();
                    Changed();
                }
                break;
            case 1:
                e.editFocus = true;
                SDL_StartTextInput(g.window);
                break;
            case 2: TakeArt(); break;
            case 3:
                if (const AlbumGroup* group = Group(); group && group->artTrack != kInvalidTrack &&
                    ClearAlbumGroupArt(e.group)) { Refresh(); Changed(); }
                break;
            case 4:
                if (const AlbumGroup* group = Group(); group && group->editions.size() > 1 &&
                    RemoveEditionFromGroup(e.group, e.selected)) { Refresh(); Changed(); }
                break;
            case 5:
                if (const AlbumGroup* group = Group(); group && group->editions.size() > 1 &&
                    SplitAlbumGroup(e.group)) { ClosePanel(); Changed(); }
                break;
            case 6:
                if (const AlbumGroup* group = Group(); group && group->editions.size() > 1) OpenCompare();
                break;
            case 7:
                if (const AlbumGroup* group = Group(); group && group->manual &&
                    AllowAutomaticGrouping(e.group)) { Refresh(); Changed(); }
                break;
            case 8: ClosePanel(); break;
        }
        g.redraw = true;
        return;
    }
}

void EditionsKey(const SDL_KeyboardEvent& key) {
    const bool ctrl = key.mod & SDL_KMOD_CTRL;
    if (e.editFocus) {
        if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
            ApplyRename();
            e.editFocus = false;
            SDL_StopTextInput(g.window);
        } else if (key.key == SDLK_ESCAPE) {
            e.editFocus = false;
            SDL_StopTextInput(g.window);
        } else if (key.key == SDLK_BACKSPACE && e.caret) {
            e.edit.erase(--e.caret, 1);
        } else if (key.key == SDLK_DELETE && e.caret < e.edit.size()) {
            e.edit.erase(e.caret, 1);
        } else if (key.key == SDLK_LEFT && e.caret) --e.caret;
        else if (key.key == SDLK_RIGHT && e.caret < e.edit.size()) ++e.caret;
        else if (key.key == SDLK_HOME) e.caret = 0;
        else if (key.key == SDLK_END) e.caret = e.edit.size();
        else if (key.key == SDLK_A && ctrl) { e.edit.clear(); e.caret = 0; }
        g.redraw = true;
        return;
    }
    if (key.key == SDLK_ESCAPE) {
        if (e.compare) BackToManager();
        else ClosePanel();
        return;
    }
    if (e.compare) {
        if (key.key == SDLK_UP || key.key == SDLK_DOWN) {
            if (!e.rows.empty()) {
                e.row = std::clamp(e.row + (key.key == SDLK_DOWN ? 1 : -1), 0, (int)e.rows.size() - 1);
                g.redraw = true;
            }
        } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
            ApplyCompare(true);
        }
        return;
    }
    if (key.key == SDLK_UP || key.key == SDLK_DOWN) {
        if (!e.editions.empty()) {
            int row = std::clamp(e.row + (key.key == SDLK_DOWN ? 1 : -1), 0, (int)e.editions.size() - 1);
            e.row = row;
            e.selected = e.editions[(size_t)row];
            const AlbumEdition* edition = Edition(e.selected);
            if (edition) {
                e.edit = edition->label.empty() ? EditionLabel(*edition) : FromUtf8(edition->label);
                e.caret = e.edit.size();
            }
            g.redraw = true;
        }
    } else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) {
        if (SetPreferredEdition(e.group, e.selected)) {
            CanvasChooseEdition(e.group, e.selected);
            Refresh();
            Changed();
        }
    } else if (key.key == SDLK_TAB) {
        e.editFocus = true;
        SDL_StartTextInput(g.window);
        g.redraw = true;
    }
}

void EditionsText(const std::string& text) {
    if (!e.editFocus) return;
    std::wstring wide = FromUtf8(text);
    e.edit.insert(std::min(e.caret, e.edit.size()), wide);
    e.caret += wide.size();
    g.redraw = true;
}

void EditionsWheel(float x, float y, float dy) {
    if (e.compare || !ManagerList().In(x, y)) return;
    float rows = (float)e.editions.size() * S(30) - ManagerList().h + S(8);
    e.scroll = std::clamp(e.scroll - dy * S(96), 0.0f, std::max(0.0f, rows));
    g.redraw = true;
}

bool EditionsWantsText(float x, float y) { return !e.compare && ManagerEdit().In(x, y); }

void EditionsClose() {
    e = EditionsState{};
    SDL_StopTextInput(g.window);
}

}  // namespace lx
