#pragma once
namespace SourcePort {
// An owned, isolated application save namespace, never a previous build's root.
// Existing directories without our ownership marker are rejected unchanged.
bool ConfigureSaveRoot(const char* directory);
bool SaveStorageReady();
unsigned OpenSaveTransactions();
}
