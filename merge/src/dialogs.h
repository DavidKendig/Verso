#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

bool RegisterDialogClasses(HINSTANCE inst);

// Modal single-line text prompt. Returns nullopt when cancelled.
std::optional<std::wstring> PromptText(HWND owner, const std::wstring& title, const std::wstring& label,
                                       const std::wstring& initial = L"");

// Modal filterable list (used for the command palette and branch pickers).
// Returns the index into `items`, or -1 when cancelled.
int PickFromList(HWND owner, const std::wstring& title, const std::vector<std::wstring>& items,
                 const std::wstring& initialFilter = L"");

std::optional<std::wstring> ChooseFolder(HWND owner, const std::wstring& title);

bool Confirm(HWND owner, const std::wstring& text, const std::wstring& title = L"Confirm");
