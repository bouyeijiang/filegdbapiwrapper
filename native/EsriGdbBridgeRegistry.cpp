//
// EsriGdbBridgeRegistry.cpp
//
// Handle tables and object lifetime: geodatabases, tables and cursors are
// registered here and torn down cascade-wise (geodatabase -> tables ->
// cursors) when an owner is closed.
//

#include "EsriGdbBridge.h"

#include <cstring>

namespace esrigdb_bridge {


// ---------------------------------------------------------------------------
// Handle table
// ---------------------------------------------------------------------------
std::mutex                       g_mutex;
std::map<egdb_handle, Geodatabase*> g_geodatabases;
egdb_handle                      g_nextHandle = 1;

// All live cursors, plus an index by owning geodatabase / table so that
// closing them can tear down the cursors still streaming from them.
std::set<egdb_cursor*>                               g_liveCursors;
std::map<egdb_handle, std::vector<egdb_cursor*> >    g_cursorsByGdb;
std::map<egdb_tableobj*, std::vector<egdb_cursor*> > g_cursorsByTable;

// All live tables, indexed by owning geodatabase.
std::set<egdb_tableobj*>                             g_liveTables;
std::map<egdb_handle, std::vector<egdb_tableobj*> >  g_tablesByGdb;

Geodatabase* FindGeodatabase(egdb_handle handle)
{
  std::map<egdb_handle, Geodatabase*>::iterator it = g_geodatabases.find(handle);
  return (it == g_geodatabases.end()) ? NULL : it->second;
}

/// Removes `item` from `vector`, if present.
template<typename T>
void EraseFromVector(std::vector<T>& vector, const T& item)
{
  for (size_t i = 0; i < vector.size(); ++i)
  {
    if (vector[i] == item)
    {
      vector.erase(vector.begin() + static_cast<long>(i));
      return;
    }
  }
}

/// Destroys a cursor. Caller must hold g_mutex; the cursor's own mutex is
/// taken while the query is torn down.
void DestroyCursorLocked(egdb_cursor* cursor)
{
  if (cursor == NULL)
    return;

  {
    std::lock_guard<std::mutex> cursorLock(cursor->mutex);
    if (!cursor->closed)
    {
      cursor->closed = true;
      cursor->enumRows.Close();
    }
  }

  g_liveCursors.erase(cursor);

  std::map<egdb_handle, std::vector<egdb_cursor*> >::iterator byOwner =
    g_cursorsByGdb.find(cursor->ownerHandle);
  if (byOwner != g_cursorsByGdb.end())
  {
    EraseFromVector(byOwner->second, cursor);
    if (byOwner->second.empty())
      g_cursorsByGdb.erase(byOwner);
  }

  if (cursor->ownerTable != NULL)
  {
    std::map<egdb_tableobj*, std::vector<egdb_cursor*> >::iterator byTable =
      g_cursorsByTable.find(cursor->ownerTable);
    if (byTable != g_cursorsByTable.end())
    {
      EraseFromVector(byTable->second, cursor);
      if (byTable->second.empty())
        g_cursorsByTable.erase(byTable);
    }
  }

  delete cursor;
}

/// Destroys a table: first its cursors, then the FileGDB table itself.
/// Caller must hold g_mutex.
void DestroyTableLocked(egdb_tableobj* tableObj)
{
  if (tableObj == NULL)
    return;

  std::map<egdb_tableobj*, std::vector<egdb_cursor*> >::iterator cursors =
    g_cursorsByTable.find(tableObj);
  if (cursors != g_cursorsByTable.end())
  {
    std::vector<egdb_cursor*> pending = cursors->second;   // copy: Destroy modifies it
    for (size_t i = 0; i < pending.size(); ++i)
      DestroyCursorLocked(pending[i]);
  }

  {
    std::lock_guard<std::mutex> tableLock(tableObj->mutex);
    if (!tableObj->closed)
    {
      tableObj->closed = true;
      if (tableObj->geodatabase != NULL && tableObj->table != NULL)
        tableObj->geodatabase->CloseTable(*tableObj->table);
      delete tableObj->table;
      tableObj->table = NULL;
    }
  }

  g_liveTables.erase(tableObj);

  std::map<egdb_handle, std::vector<egdb_tableobj*> >::iterator byOwner =
    g_tablesByGdb.find(tableObj->gdbHandle);
  if (byOwner != g_tablesByGdb.end())
  {
    EraseFromVector(byOwner->second, tableObj);
    if (byOwner->second.empty())
      g_tablesByGdb.erase(byOwner);
  }

  delete tableObj;
}

/// True when the pointer refers to a cursor that is still open.
bool IsLiveCursor(egdb_cursor* cursor)
{
  if (cursor == NULL)
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_liveCursors.find(cursor) != g_liveCursors.end();
}

// ---------------------------------------------------------------------------
// Cursor / table metadata helpers
// ---------------------------------------------------------------------------
/// Reads the column metadata of an enumerator into the cursor.
/// Returns false when the metadata cannot be read.
bool LoadCursorMetadata(egdb_cursor* cursor)
{
  cursor->fieldNames.clear();
  cursor->fieldTypes.clear();
  cursor->fieldLengths.clear();
  cursor->fieldNullable.clear();

  FieldInfo fieldInfo;
  int       fieldCount = 0;
  if (cursor->enumRows.GetFieldInformation(fieldInfo) == S_OK)
    fieldInfo.GetFieldCount(fieldCount);

  if (fieldCount <= 0)
    return true;                       // no result set (DDL / DML statement)

  std::wstring fieldName;
  FieldType    fieldType;
  int          fieldLength = 0;
  bool         isNullable  = true;

  for (int i = 0; i < fieldCount; ++i)
  {
    if (fieldInfo.GetFieldName(i, fieldName) != S_OK ||
        fieldInfo.GetFieldType(i, fieldType) != S_OK)
      return false;

    fieldLength = -1;
    isNullable  = true;
    fieldInfo.GetFieldLength(i, fieldLength);        // best effort
    fieldInfo.GetFieldIsNullable(i, isNullable);

    cursor->fieldNames.push_back(WideToUtf8(fieldName));
    cursor->fieldTypes.push_back(static_cast<int32_t>(fieldType));
    cursor->fieldLengths.push_back(static_cast<int32_t>(fieldLength));
    cursor->fieldNullable.push_back(isNullable ? 1 : 0);
  }

  cursor->hasResultSet = true;
  return true;
}

/// Publishes a cursor into the registries. Caller must hold g_mutex.
void RegisterCursorLocked(egdb_cursor* cursor)
{
  g_liveCursors.insert(cursor);
  g_cursorsByGdb[cursor->ownerHandle].push_back(cursor);
  if (cursor->ownerTable != NULL)
    g_cursorsByTable[cursor->ownerTable].push_back(cursor);
}

// ---------------------------------------------------------------------------
// Table field metadata cache
// ---------------------------------------------------------------------------
/// (Re)loads the cached field metadata of a table. Caller holds table->mutex.
bool RefreshFieldCache(egdb_tableobj* tableObj)
{
  tableObj->fieldNames.clear();
  tableObj->fieldTypes.clear();
  tableObj->fieldLengths.clear();
  tableObj->fieldNullable.clear();

  if (tableObj->table == NULL)
    return false;

  FieldInfo fieldInfo;
  if (tableObj->table->GetFieldInformation(fieldInfo) != S_OK)
    return false;

  int fieldCount = 0;
  if (fieldInfo.GetFieldCount(fieldCount) != S_OK)
    return false;

  std::wstring fieldName;
  FieldType    fieldType;
  int          fieldLength = -1;
  bool         isNullable  = true;

  for (int i = 0; i < fieldCount; ++i)
  {
    if (fieldInfo.GetFieldName(i, fieldName) != S_OK ||
        fieldInfo.GetFieldType(i, fieldType) != S_OK)
      return false;

    fieldLength = -1;
    isNullable  = true;
    fieldInfo.GetFieldLength(i, fieldLength);
    fieldInfo.GetFieldIsNullable(i, isNullable);

    tableObj->fieldNames.push_back(WideToUtf8(fieldName));
    tableObj->fieldTypes.push_back(static_cast<int32_t>(fieldType));
    tableObj->fieldLengths.push_back(static_cast<int32_t>(fieldLength));
    tableObj->fieldNullable.push_back(isNullable ? 1 : 0);
  }
  return true;
}

/// Allocates a table handle and registers it. Caller must hold g_mutex.
egdb_result PublishTable(Geodatabase* geodatabase, Table* table,
                         egdb_handle handle, const std::wstring& path,
                         egdb_tableobj** outTable,
                         char* errBuf, int32_t errBufLen)
{
  egdb_tableobj* tableObj = new (std::nothrow) egdb_tableobj_t();
  if (tableObj == NULL)
  {
    delete table;
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  }

  tableObj->geodatabase = geodatabase;
  tableObj->table       = table;
  tableObj->gdbHandle   = handle;
  tableObj->path        = path;

  {
    std::lock_guard<std::mutex> tableLock(tableObj->mutex);
    if (!RefreshFieldCache(tableObj))
    {
      delete tableObj->table;
      delete tableObj;
      return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "failed to read the table fields");
    }
  }

  g_liveTables.insert(tableObj);
  g_tablesByGdb[handle].push_back(tableObj);
  *outTable = tableObj;
  return EGDB_OK;
}

} // namespace esrigdb_bridge
