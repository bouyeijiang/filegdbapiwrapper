//
// EsriGdbNative.cpp
//
// The public C ABI of the bridge: the only file that exports entry points
// (see EsriGdbNative.h). The helpers it uses live in the EsriGdbBridge*.cpp
// files, declared in EsriGdbBridge.h.
//
#include "EsriGdbBridge.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

// The bridge helpers are deliberately used unqualified in this file: it
// only speaks the C ABI.
using namespace esrigdb_bridge;

// ---------------------------------------------------------------------------
// Public C ABI
// ---------------------------------------------------------------------------
extern "C" {

EGDB_EXPORT egdb_result egdb_open(const char* utf8Path, egdb_handle* outHandle,
                                  char* errBuf, int32_t errBufLen)
{
  if (outHandle == NULL)
    return SetArgumentError(errBuf, errBufLen, "outHandle must not be NULL");
  *outHandle = 0;

  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "geodatabase path is empty");

  const std::wstring path = Utf8ToWide(utf8Path);

  Geodatabase* geodatabase = new (std::nothrow) Geodatabase();
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "out of memory");

  fgdbError hr = OpenGeodatabase(path, *geodatabase);
  if (hr != S_OK)
  {
    delete geodatabase;
    return SetLastError(errBuf, errBufLen, hr, "failed to open the geodatabase");
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  *outHandle = g_nextHandle++;
  g_geodatabases[*outHandle] = geodatabase;
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_close(egdb_handle handle, char* errBuf, int32_t errBufLen)
{
  Geodatabase* geodatabase = NULL;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::map<egdb_handle, Geodatabase*>::iterator it = g_geodatabases.find(handle);
    if (it == g_geodatabases.end())
      return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");
    geodatabase = it->second;
    g_geodatabases.erase(it);

    // Tear down every table (and their cursors) still open on this
    // geodatabase. Destroying a table first closes the cursors streaming
    // from it, so a second pass catches the SQL cursors.
    std::map<egdb_handle, std::vector<egdb_tableobj*> >::iterator tables =
      g_tablesByGdb.find(handle);
    if (tables != g_tablesByGdb.end())
    {
      std::vector<egdb_tableobj*> pending = tables->second;   // copy: Destroy modifies it
      for (size_t i = 0; i < pending.size(); ++i)
        DestroyTableLocked(pending[i]);
    }

    // Tear down every cursor still streaming from this geodatabase.
    std::map<egdb_handle, std::vector<egdb_cursor*> >::iterator cursors =
      g_cursorsByGdb.find(handle);
    if (cursors != g_cursorsByGdb.end())
    {
      std::vector<egdb_cursor*> pending = cursors->second;   // copy: Destroy modifies it
      for (size_t i = 0; i < pending.size(); ++i)
        DestroyCursorLocked(pending[i]);
    }
  }

  fgdbError hr = CloseGeodatabase(*geodatabase);
  delete geodatabase;
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to close the geodatabase");

  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_execute_sql(egdb_handle handle, const char* utf8Sql,
                                         egdb_table* outTable, char* errBuf, int32_t errBufLen)
{
  if (outTable == NULL)
    return SetArgumentError(errBuf, errBufLen, "outTable must not be NULL");
  InitTable(outTable);

  // The materialising API is a thin loop over the streaming API.
  egdb_cursor*  cursor = NULL;
  egdb_result   status = egdb_execute_sql_cursor(handle, utf8Sql, &cursor, errBuf, errBufLen);
  if (status != EGDB_OK)
    return status;

  const int32_t       fieldCount = egdb_cursor_field_count(cursor);
  std::vector<char*>  names(static_cast<size_t>(fieldCount), NULL);
  std::vector<int32_t> types(static_cast<size_t>(fieldCount), 0);

  for (int32_t i = 0; i < fieldCount; ++i)
  {
    const char* name = egdb_cursor_field_name(cursor, i);
    names[static_cast<size_t>(i)] = DupString(name != NULL ? name : "");
    types[static_cast<size_t>(i)] = egdb_cursor_field_type(cursor, i);
  }

  std::vector<char*> cells;
  int32_t             rowCount = 0;

  for (;;)
  {
    egdb_row row;
    row.field_count = 0;
    row.values      = NULL;

    status = egdb_cursor_next(cursor, &row, errBuf, errBufLen);
    if (status == EGDB_CURSOR_END)
      break;

    if (status != EGDB_CURSOR_ROW)
    {
      FreeStrings(names);
      FreeStrings(cells);
      egdb_cursor_close(cursor);
      return status;
    }

    // Take ownership of the row's value pointers instead of copying them.
    for (int32_t i = 0; i < row.field_count && i < fieldCount; ++i)
      cells.push_back(row.values[i]);
    for (int32_t i = row.field_count; i < fieldCount; ++i)
      cells.push_back(NULL);
    free(row.values);
    row.values = NULL;
    ++rowCount;
  }

  egdb_cursor_close(cursor);

  outTable->field_count = fieldCount;
  outTable->row_count   = rowCount;
  outTable->field_names = DupPointerArray(names);
  outTable->field_types = DupIntArray(types);
  outTable->values      = DupPointerArray(cells);
  return EGDB_OK;
}

EGDB_EXPORT void egdb_free_table(egdb_table* table)
{
  if (table == NULL)
    return;

  if (table->field_names != NULL)
  {
    for (int32_t i = 0; i < table->field_count; ++i)
      free(table->field_names[i]);
    free(table->field_names);
  }

  if (table->values != NULL)
  {
    const int32_t total = table->field_count * table->row_count;
    for (int32_t i = 0; i < total; ++i)
      free(table->values[i]);
    free(table->values);
  }

  free(table->field_types);
  InitTable(table);
}

// ---------------------------------------------------------------------------
// Cursor (streaming) API
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_execute_sql_cursor(egdb_handle handle, const char* utf8Sql,
                                                egdb_cursor** outCursor,
                                                char* errBuf, int32_t errBufLen)
{
  if (outCursor == NULL)
    return SetArgumentError(errBuf, errBufLen, "outCursor must not be NULL");
  *outCursor = NULL;

  if (utf8Sql == NULL || *utf8Sql == '\0')
    return SetArgumentError(errBuf, errBufLen, "SQL statement is empty");

  // The registry lock also guarantees that the geodatabase cannot disappear
  // while the query is being set up.
  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  egdb_cursor* cursor = new (std::nothrow) egdb_cursor_t();
  if (cursor == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");

  cursor->geodatabase = geodatabase;
  cursor->ownerHandle = handle;

  fgdbError hr = geodatabase->ExecuteSQL(Utf8ToWide(utf8Sql), true, cursor->enumRows);
  if (hr != S_OK)
  {
    delete cursor;
    return SetLastError(errBuf, errBufLen, hr, "the SQL statement failed");
  }

  // Column metadata (available straight away, before the first row is read).
  if (!LoadCursorMetadata(cursor))
  {
    delete cursor;
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "failed to read field metadata");
  }

  if (!cursor->hasResultSet)
  {
    // INSERT / UPDATE / DELETE / DDL - no result set, next() ends immediately.
    cursor->ended = true;
    cursor->enumRows.Close();
  }

  RegisterCursorLocked(cursor);

  *outCursor = cursor;
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_cursor_next(egdb_cursor* cursor, egdb_row* outRow,
                                         char* errBuf, int32_t errBufLen)
{
  if (outRow == NULL)
    return SetArgumentError(errBuf, errBufLen, "outRow must not be NULL");
  outRow->field_count = 0;
  outRow->values      = NULL;

  // Validity check happens *before* locking the cursor so that the registry
  // lock and the cursor lock are never held at the same time here.
  if (!IsLiveCursor(cursor))
    return SetArgumentError(errBuf, errBufLen, "cursor is closed or invalid");

  std::lock_guard<std::mutex> lock(cursor->mutex);

  if (cursor->closed)
    return SetArgumentError(errBuf, errBufLen, "cursor is closed");
  if (cursor->ended || !cursor->hasResultSet)
    return EGDB_CURSOR_END;

  const fgdbError hr = cursor->enumRows.Next(cursor->row);
  if (hr == S_FALSE)
  {
    cursor->ended = true;
    return EGDB_CURSOR_END;
  }
  if (hr != S_OK)
  {
    cursor->ended = true;
    return SetLastError(errBuf, errBufLen, hr, "failed to fetch the next row");
  }

  const int32_t       fieldCount = static_cast<int32_t>(cursor->fieldTypes.size());
  std::vector<char*>  values(static_cast<size_t>(fieldCount), NULL);

  for (int32_t i = 0; i < fieldCount; ++i)
  {
    std::string value;
    bool        isNull = false;

    if (!ExtractValue(cursor->row, i, static_cast<FieldType>(cursor->fieldTypes[static_cast<size_t>(i)]),
                      value, isNull))
    {
      FreeStrings(values);
      return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "failed to read a row value");
    }
    values[static_cast<size_t>(i)] = isNull ? NULL : DupString(value);
  }

  char** array = DupPointerArray(values);
  if (array == NULL && fieldCount > 0)
  {
    FreeStrings(values);
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  }

  outRow->field_count = fieldCount;
  outRow->values      = array;
  return EGDB_CURSOR_ROW;
}

EGDB_EXPORT int32_t egdb_cursor_field_count(const egdb_cursor* cursor)
{
  egdb_cursor* mutableCursor = const_cast<egdb_cursor*>(cursor);
  if (!IsLiveCursor(mutableCursor))
    return 0;

  std::lock_guard<std::mutex> lock(mutableCursor->mutex);
  return static_cast<int32_t>(mutableCursor->fieldNames.size());
}

EGDB_EXPORT const char* egdb_cursor_field_name(const egdb_cursor* cursor, int32_t index)
{
  egdb_cursor* mutableCursor = const_cast<egdb_cursor*>(cursor);
  if (!IsLiveCursor(mutableCursor))
    return NULL;

  std::lock_guard<std::mutex> lock(mutableCursor->mutex);
  if (index < 0 || static_cast<size_t>(index) >= mutableCursor->fieldNames.size())
    return NULL;

  return mutableCursor->fieldNames[static_cast<size_t>(index)].c_str();
}

EGDB_EXPORT int32_t egdb_cursor_field_type(const egdb_cursor* cursor, int32_t index)
{
  egdb_cursor* mutableCursor = const_cast<egdb_cursor*>(cursor);
  if (!IsLiveCursor(mutableCursor))
    return -1;

  std::lock_guard<std::mutex> lock(mutableCursor->mutex);
  if (index < 0 || static_cast<size_t>(index) >= mutableCursor->fieldTypes.size())
    return -1;

  return mutableCursor->fieldTypes[static_cast<size_t>(index)];
}

EGDB_EXPORT void egdb_cursor_close(egdb_cursor* cursor)
{
  if (cursor == NULL)
    return;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_liveCursors.find(cursor) == g_liveCursors.end())
    return;                                    // already closed

  DestroyCursorLocked(cursor);
}

EGDB_EXPORT void egdb_free_row(egdb_row* row)
{
  if (row == NULL)
    return;

  if (row->values != NULL)
  {
    for (int32_t i = 0; i < row->field_count; ++i)
      free(row->values[i]);
    free(row->values);
  }

  row->field_count = 0;
  row->values      = NULL;
}

// ---------------------------------------------------------------------------
// Common helpers: strings / string arrays / envelopes
// ---------------------------------------------------------------------------

EGDB_EXPORT void egdb_free_string(char* str)
{
  free(str);
}

EGDB_EXPORT void egdb_free_string_array(egdb_string_array* array)
{
  if (array == NULL)
    return;

  if (array->items != NULL)
  {
    for (int32_t i = 0; i < array->count; ++i)
      free(array->items[i]);
    free(array->items);
  }

  array->count = 0;
  array->items = NULL;
}

// ---------------------------------------------------------------------------
// Geodatabase management
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_create_geodatabase(const char* utf8Path, egdb_handle* outHandle,
                                                char* errBuf, int32_t errBufLen)
{
  if (outHandle == NULL)
    return SetArgumentError(errBuf, errBufLen, "outHandle must not be NULL");
  *outHandle = 0;

  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "geodatabase path is empty");

  const std::wstring path = Utf8ToWide(utf8Path);

  Geodatabase* geodatabase = new (std::nothrow) Geodatabase();
  if (geodatabase == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");

  const fgdbError hr = CreateGeodatabase(path, *geodatabase);
  if (hr != S_OK)
  {
    delete geodatabase;
    return SetLastError(errBuf, errBufLen, hr, "failed to create the geodatabase");
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  *outHandle = g_nextHandle++;
  g_geodatabases[*outHandle] = geodatabase;
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_delete_geodatabase(const char* utf8Path,
                                                char* errBuf, int32_t errBufLen)
{
  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "geodatabase path is empty");

  const fgdbError hr = DeleteGeodatabase(Utf8ToWide(utf8Path));
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to delete the geodatabase");
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_compact_database(egdb_handle handle,
                                              char* errBuf, int32_t errBufLen)
{
  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  const fgdbError hr = geodatabase->CompactDatabase();
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to compact the geodatabase");
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Catalog browsing
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_get_dataset_types(egdb_handle handle,
                                               egdb_string_array* outTypes,
                                               char* errBuf, int32_t errBufLen)
{
  if (outTypes == NULL)
    return SetArgumentError(errBuf, errBufLen, "outTypes must not be NULL");
  outTypes->count = 0;
  outTypes->items = NULL;

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  std::vector<std::wstring> types;
  const fgdbError hr = geodatabase->GetDatasetTypes(types);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to list the dataset types");

  std::vector<std::string> utf8;
  utf8.reserve(types.size());
  for (size_t i = 0; i < types.size(); ++i)
    utf8.push_back(WideToUtf8(types[i]));

  FillStringArray(utf8, outTypes);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_get_child_datasets(egdb_handle handle,
                                                const char* utf8ParentPath,
                                                const char* utf8DatasetType,
                                                egdb_string_array* outChildren,
                                                char* errBuf, int32_t errBufLen)
{
  if (outChildren == NULL)
    return SetArgumentError(errBuf, errBufLen, "outChildren must not be NULL");
  outChildren->count = 0;
  outChildren->items = NULL;

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  const std::wstring parentPath = RootedPath(utf8ParentPath);   // "" -> "\" (the root)
  const std::wstring datasetType = Utf8ToWide(utf8DatasetType != NULL ? utf8DatasetType : "");

  std::vector<std::wstring> children;
  const fgdbError hr = geodatabase->GetChildDatasets(parentPath, datasetType, children);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to list the child datasets");

  std::vector<std::string> utf8;
  utf8.reserve(children.size());
  for (size_t i = 0; i < children.size(); ++i)
    utf8.push_back(WideToUtf8(children[i]));

  FillStringArray(utf8, outChildren);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_get_query_name(egdb_handle handle, const char* utf8Path,
                                            char** outName,
                                            char* errBuf, int32_t errBufLen)
{
  if (outName == NULL)
    return SetArgumentError(errBuf, errBufLen, "outName must not be NULL");
  *outName = NULL;

  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "dataset path is empty");

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  std::wstring queryName;
  const fgdbError hr = geodatabase->GetQueryName(RootedPath(utf8Path), queryName);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to resolve the query name");

  *outName = DupString(WideToUtf8(queryName));
  if (*outName == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_get_dataset_definition(egdb_handle handle,
                                                    const char* utf8Path,
                                                    const char* utf8DatasetType,
                                                    char** outXml,
                                                    char* errBuf, int32_t errBufLen)
{
  if (outXml == NULL)
    return SetArgumentError(errBuf, errBufLen, "outXml must not be NULL");
  *outXml = NULL;

  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "dataset path is empty");

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  const std::wstring path       = RootedPath(utf8Path);
  const std::wstring datasetType = Utf8ToWide(utf8DatasetType != NULL ? utf8DatasetType : "");

  std::string definition;
  const fgdbError hr = geodatabase->GetDatasetDefinition(path, datasetType, definition);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to read the dataset definition");

  *outXml = DupString(definition);
  if (*outXml == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_get_domains(egdb_handle handle,
                                         egdb_string_array* outNames,
                                         char* errBuf, int32_t errBufLen)
{
  if (outNames == NULL)
    return SetArgumentError(errBuf, errBufLen, "outNames must not be NULL");
  outNames->count = 0;
  outNames->items = NULL;

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  std::vector<std::wstring> names;
  const fgdbError hr = geodatabase->GetDomains(names);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to list the domains");

  std::vector<std::string> utf8;
  utf8.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i)
    utf8.push_back(WideToUtf8(names[i]));

  FillStringArray(utf8, outNames);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_get_domain_definition(egdb_handle handle,
                                                   const char* utf8DomainName,
                                                   char** outXml,
                                                   char* errBuf, int32_t errBufLen)
{
  if (outXml == NULL)
    return SetArgumentError(errBuf, errBufLen, "outXml must not be NULL");
  *outXml = NULL;

  if (utf8DomainName == NULL || *utf8DomainName == '\0')
    return SetArgumentError(errBuf, errBufLen, "domain name is empty");

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  std::string definition;
  const fgdbError hr = geodatabase->GetDomainDefinition(Utf8ToWide(utf8DomainName), definition);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to read the domain definition");

  *outXml = DupString(definition);
  if (*outXml == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_create_domain(egdb_handle handle, const char* utf8Xml,
                                           char* errBuf, int32_t errBufLen)
{
  if (utf8Xml == NULL || *utf8Xml == '\0')
    return SetArgumentError(errBuf, errBufLen, "the domain definition is empty");

  std::string NormalizedXml(utf8Xml);
  NormalizeXmlFragment(NormalizedXml, "esri:Domain");   // domains always use <esri:Domain>

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  const fgdbError hr = geodatabase->CreateDomain(NormalizedXml);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to create the domain");
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_delete_domain(egdb_handle handle, const char* utf8DomainName,
                                           char* errBuf, int32_t errBufLen)
{
  if (utf8DomainName == NULL || *utf8DomainName == '\0')
    return SetArgumentError(errBuf, errBufLen, "domain name is empty");

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  const fgdbError hr = geodatabase->DeleteDomain(Utf8ToWide(utf8DomainName));
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to delete the domain");
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Tables: open / create / close
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_open_table(egdb_handle handle, const char* utf8Path,
                                        egdb_tableobj** outTable,
                                        char* errBuf, int32_t errBufLen)
{
  if (outTable == NULL)
    return SetArgumentError(errBuf, errBufLen, "outTable must not be NULL");
  *outTable = NULL;

  if (utf8Path == NULL || *utf8Path == '\0')
    return SetArgumentError(errBuf, errBufLen, "table path is empty");

  const std::wstring path = RootedPath(utf8Path);

  // The registry lock also guarantees that the geodatabase cannot disappear
  // while the table is being opened.
  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  Table* table = new (std::nothrow) Table();
  if (table == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");

  const fgdbError hr = geodatabase->OpenTable(path, *table);
  if (hr != S_OK)
  {
    delete table;
    return SetLastError(errBuf, errBufLen, hr, "failed to open the table");
  }

  return PublishTable(geodatabase, table, handle, path, outTable, errBuf, errBufLen);
}

EGDB_EXPORT egdb_result egdb_create_table(egdb_handle handle,
                                          const char* utf8TableDefXml,
                                          const char* utf8ParentPath,
                                          egdb_tableobj** outTable,
                                          char* errBuf, int32_t errBufLen)
{
  if (outTable == NULL)
    return SetArgumentError(errBuf, errBufLen, "outTable must not be NULL");
  *outTable = NULL;

  if (utf8TableDefXml == NULL || *utf8TableDefXml == '\0')
    return SetArgumentError(errBuf, errBufLen, "the table definition is empty");

  const std::wstring parentRaw = Utf8ToWide(utf8ParentPath != NULL ? utf8ParentPath : "");
  const std::wstring parent = parentRaw.empty()
    ? std::wstring()                          // "" keeps the dataset at the root
    : RootedPath(utf8ParentPath);

  std::string tableDefXml(utf8TableDefXml);
  NormalizeXmlFragment(tableDefXml, NULL);    // esri: prefix + namespace declarations

  std::string problem;
  if (!ValidateTableDefinition(tableDefXml, problem))
    return SetArgumentError(errBuf, errBufLen, problem.c_str());

  std::lock_guard<std::mutex> lock(g_mutex);

  Geodatabase* geodatabase = FindGeodatabase(handle);
  if (geodatabase == NULL)
    return SetArgumentError(errBuf, errBufLen, "invalid geodatabase handle");

  Table* table = new (std::nothrow) Table();
  if (table == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");

  const fgdbError hr = geodatabase->CreateTable(tableDefXml, parent, *table);
  if (hr != S_OK)
  {
    delete table;
    return SetLastError(errBuf, errBufLen, hr, "failed to create the table");
  }

  return PublishTable(geodatabase, table, handle, parent, outTable, errBuf, errBufLen);
}

EGDB_EXPORT egdb_result egdb_close_table(egdb_handle handle, egdb_tableobj* table,
                                         char* errBuf, int32_t errBufLen)
{
  if (table == NULL)
    return SetArgumentError(errBuf, errBufLen, "table must not be NULL");

  std::lock_guard<std::mutex> lock(g_mutex);

  if (g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");
  if (table->gdbHandle != handle)
    return SetArgumentError(errBuf, errBufLen, "the table belongs to another geodatabase");

  DestroyTableLocked(table);
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Tables: schema and metadata
// ---------------------------------------------------------------------------

EGDB_EXPORT int32_t egdb_table_field_count(egdb_tableobj* table)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return 0;

  std::lock_guard<std::mutex> tableLock(table->mutex);
  return static_cast<int32_t>(table->fieldNames.size());
}

EGDB_EXPORT const char* egdb_table_field_name(egdb_tableobj* table, int32_t index)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return NULL;

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (index < 0 || static_cast<size_t>(index) >= table->fieldNames.size())
    return NULL;

  return table->fieldNames[static_cast<size_t>(index)].c_str();
}

EGDB_EXPORT int32_t egdb_table_field_type(egdb_tableobj* table, int32_t index)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return -1;

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (index < 0 || static_cast<size_t>(index) >= table->fieldTypes.size())
    return -1;

  return table->fieldTypes[static_cast<size_t>(index)];
}

EGDB_EXPORT int32_t egdb_table_field_length(egdb_tableobj* table, int32_t index)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return -2;

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (index < 0 || static_cast<size_t>(index) >= table->fieldLengths.size())
    return -2;

  return table->fieldLengths[static_cast<size_t>(index)];
}

EGDB_EXPORT int32_t egdb_table_field_nullable(egdb_tableobj* table, int32_t index)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return -1;

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (index < 0 || static_cast<size_t>(index) >= table->fieldNullable.size())
    return -1;

  return table->fieldNullable[static_cast<size_t>(index)];
}

EGDB_EXPORT egdb_result egdb_table_get_definition(egdb_tableobj* table, char** outXml,
                                                  char* errBuf, int32_t errBufLen)
{
  if (outXml == NULL)
    return SetArgumentError(errBuf, errBufLen, "outXml must not be NULL");
  *outXml = NULL;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (table->table == NULL)
    return SetArgumentError(errBuf, errBufLen, "the table is closed");

  std::string definition;
  const fgdbError hr = table->table->GetDefinition(definition);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to read the table definition");

  *outXml = DupString(definition);
  if (*outXml == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_table_add_field(egdb_tableobj* table, const char* utf8FieldDefXml,
                                             char* errBuf, int32_t errBufLen)
{
  if (utf8FieldDefXml == NULL || *utf8FieldDefXml == '\0')
    return SetArgumentError(errBuf, errBufLen, "the field definition is empty");

  std::string fieldDefXml(utf8FieldDefXml);
  NormalizeXmlFragment(fieldDefXml, "esri:Field");

  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (table->table == NULL)
    return SetArgumentError(errBuf, errBufLen, "the table is closed");

  const fgdbError hr = table->table->AddField(fieldDefXml);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to add the field");

  RefreshFieldCache(table);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_table_delete_field(egdb_tableobj* table, const char* utf8FieldName,
                                                char* errBuf, int32_t errBufLen)
{
  if (utf8FieldName == NULL || *utf8FieldName == '\0')
    return SetArgumentError(errBuf, errBufLen, "the field name is empty");

  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (table->table == NULL)
    return SetArgumentError(errBuf, errBufLen, "the table is closed");

  const fgdbError hr = table->table->DeleteField(Utf8ToWide(utf8FieldName));
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to delete the field");

  RefreshFieldCache(table);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_table_add_index(egdb_tableobj* table, const char* utf8IndexDefXml,
                                             char* errBuf, int32_t errBufLen)
{
  if (utf8IndexDefXml == NULL || *utf8IndexDefXml == '\0')
    return SetArgumentError(errBuf, errBufLen, "the index definition is empty");

  std::string indexDefXml(utf8IndexDefXml);
  NormalizeXmlFragment(indexDefXml, "esri:Index");

  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (table->table == NULL)
    return SetArgumentError(errBuf, errBufLen, "the table is closed");

  const fgdbError hr = table->table->AddIndex(indexDefXml);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to add the index");
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Tables: search (streaming) / insert / update / delete
// ---------------------------------------------------------------------------

EGDB_EXPORT egdb_result egdb_table_search(egdb_tableobj* table,
                                          const char* utf8Subfields,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          int32_t recycling,
                                          egdb_cursor** outCursor,
                                          char* errBuf, int32_t errBufLen)
{
  if (outCursor == NULL)
    return SetArgumentError(errBuf, errBufLen, "outCursor must not be NULL");
  *outCursor = NULL;

  // Both locks keep the table and its geodatabase alive for the whole setup.
  std::lock_guard<std::mutex> lock(g_mutex);
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
    return SetArgumentError(errBuf, errBufLen, "invalid table handle");

  std::lock_guard<std::mutex> tableLock(table->mutex);
  if (table->table == NULL)
    return SetArgumentError(errBuf, errBufLen, "the table is closed");

  const std::wstring subfields   = (utf8Subfields != NULL && *utf8Subfields != '\0')
                                   ? Utf8ToWide(utf8Subfields) : std::wstring(L"*");
  const std::wstring whereClause = Utf8ToWide(utf8WhereClause != NULL ? utf8WhereClause : "");

  egdb_cursor* cursor = new (std::nothrow) egdb_cursor_t();
  if (cursor == NULL)
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "out of memory");

  cursor->geodatabase = table->geodatabase;
  cursor->ownerTable  = table;
  cursor->ownerHandle = table->gdbHandle;

  fgdbError hr;
  if (HasEnvelope(envelope))
  {
    const Envelope box = ToEnvelope(envelope);
    hr = table->table->Search(subfields, whereClause, box, recycling != 0, cursor->enumRows);
  }
  else
  {
    hr = table->table->Search(subfields, whereClause, recycling != 0, cursor->enumRows);
  }

  if (hr != S_OK)
  {
    delete cursor;
    return SetLastError(errBuf, errBufLen, hr, "the search failed");
  }

  if (!LoadCursorMetadata(cursor))
  {
    delete cursor;
    return SetNativeError(errBuf, errBufLen, EGDB_E_FAIL, "failed to read field metadata");
  }

  RegisterCursorLocked(cursor);
  *outCursor = cursor;
  return EGDB_OK;
}

/// Writes (name, value) pairs into a freshly created / freshly read row.
/// Returns false with a description in errorText when a value is rejected.
static bool ApplyFieldValues(egdb_tableobj* tableObj, Row& row,
                      const char* const* names, const char* const* values,
                      int32_t count, fgdbError& hr, std::string& errorText)
{
  hr = S_OK;
  errorText.clear();

  if (count > 0 && names == NULL)
  {
    errorText = "the field name array is NULL";
    return false;
  }

  for (int32_t i = 0; i < count; ++i)
  {
    if (names[i] == NULL || *names[i] == '\0')
    {
      std::ostringstream os;
      os << "the field name at index " << i << " is empty";
      errorText = os.str();
      return false;
    }

    const int index = FindFieldIndex(tableObj, names[i]);
    if (index < 0)
    {
      errorText = std::string("unknown field '") + names[i] + "'";
      return false;
    }

    const FieldType fieldType = static_cast<FieldType>(tableObj->fieldTypes[static_cast<size_t>(index)]);
    const std::wstring fieldName = Utf8ToWide(tableObj->fieldNames[static_cast<size_t>(index)].c_str());
    const char* value = (values != NULL) ? values[i] : NULL;

    // Strings are limited to the declared field length; report it here rather
    // than letting the API reject the whole row with a generic error.
    if (value != NULL && fieldType == fieldTypeString &&
        tableObj->fieldLengths[static_cast<size_t>(index)] > 0)
    {
      const size_t maxLength = static_cast<size_t>(tableObj->fieldLengths[static_cast<size_t>(index)]);
      const size_t length = Utf8CharCount(value);
      if (length > maxLength)
      {
        std::ostringstream os;
        os << "value '" << value << "' is " << length << " characters but field '"
           << tableObj->fieldNames[static_cast<size_t>(index)] << "' allows at most "
           << maxLength;
        errorText = os.str();
        return false;
      }
    }

    if (!ApplyValue(row, fieldName, fieldType, value, hr, errorText))
      return false;
  }
  return true;
}

/// Common prologue of the write operations. Returns NULL (and reports why)
/// when the table handle is unusable; otherwise the caller keeps both locks
/// (g_mutex is already held by the caller of this helper).
static bool EnterTableOp(egdb_tableobj* table, char* errBuf, int32_t errBufLen)
{
  if (table == NULL || g_liveTables.find(table) == g_liveTables.end())
  {
    SetArgumentError(errBuf, errBufLen, "invalid table handle");
    return false;
  }
  if (table->table == NULL)
  {
    SetArgumentError(errBuf, errBufLen, "the table is closed");
    return false;
  }
  return true;
}

EGDB_EXPORT egdb_result egdb_table_insert(egdb_tableobj* table,
                                          const char* const* names,
                                          const char* const* values,
                                          int32_t count,
                                          int32_t* outNewOid,
                                          char* errBuf, int32_t errBufLen)
{
  if (outNewOid != NULL)
    *outNewOid = -1;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!EnterTableOp(table, errBuf, errBufLen))
    return EGDB_E_INVALIDARG;

  std::lock_guard<std::mutex> tableLock(table->mutex);

  Row row;
  fgdbError hr = table->table->CreateRowObject(row);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to create a row");

  std::string errorText;
  if (!ApplyFieldValues(table, row, names, values, count, hr, errorText))
    return (hr != S_OK)
      ? SetLastError(errBuf, errBufLen, hr, errorText.c_str())
      : SetNativeError(errBuf, errBufLen, EGDB_E_INVALIDARG, errorText.c_str());

  hr = table->table->Insert(row);
  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, "failed to insert the row");

  if (outNewOid != NULL)
  {
    int32 oid = -1;
    if (row.GetOID(oid) != S_OK)
      oid = -1;
    *outNewOid = oid;
  }
  return EGDB_OK;
}

/// Opens a non recycling enumerator over the rows matched by the filter.
/// The caller must close `rows`.
static egdb_result OpenRowEnumerator(egdb_tableobj* table, const char* utf8WhereClause,
                              const egdb_envelope* envelope, const char* what,
                              EnumRows& rows, char* errBuf, int32_t errBufLen)
{
  const std::wstring whereClause = Utf8ToWide(utf8WhereClause != NULL ? utf8WhereClause : "");

  fgdbError hr;
  if (HasEnvelope(envelope))
  {
    const Envelope box = ToEnvelope(envelope);
    hr = table->table->Search(L"*", whereClause, box, false, rows);
  }
  else
  {
    hr = table->table->Search(L"*", whereClause, false, rows);
  }

  if (hr != S_OK)
    return SetLastError(errBuf, errBufLen, hr, what);
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_table_update(egdb_tableobj* table,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          const char* const* names,
                                          const char* const* values,
                                          int32_t count,
                                          int32_t* outAffected,
                                          char* errBuf, int32_t errBufLen)
{
  if (outAffected != NULL)
    *outAffected = 0;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!EnterTableOp(table, errBuf, errBufLen))
    return EGDB_E_INVALIDARG;

  std::lock_guard<std::mutex> tableLock(table->mutex);

  EnumRows rows;
  egdb_result status = OpenRowEnumerator(table, utf8WhereClause, envelope,
                                         "the search for rows to update failed",
                                         rows, errBuf, errBufLen);
  if (status != EGDB_OK)
    return status;

  Row      row;
  fgdbError nextHr = S_OK;
  int32_t   affected = 0;

  while ((nextHr = rows.Next(row)) == S_OK)
  {
    std::string errorText;
    fgdbError   hr = S_OK;
    if (!ApplyFieldValues(table, row, names, values, count, hr, errorText))
    {
      rows.Close();
      return (hr != S_OK)
        ? SetLastError(errBuf, errBufLen, hr, errorText.c_str())
        : SetNativeError(errBuf, errBufLen, EGDB_E_INVALIDARG, errorText.c_str());
    }

    hr = table->table->Update(row);
    if (hr != S_OK)
    {
      rows.Close();
      return SetLastError(errBuf, errBufLen, hr, "failed to update a row");
    }
    ++affected;
  }

  rows.Close();

  if (nextHr != S_FALSE)
    return SetLastError(errBuf, errBufLen, nextHr, "failed while iterating the rows to update");

  if (outAffected != NULL)
    *outAffected = affected;
  return EGDB_OK;
}

EGDB_EXPORT egdb_result egdb_table_delete(egdb_tableobj* table,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          int32_t* outAffected,
                                          char* errBuf, int32_t errBufLen)
{
  if (outAffected != NULL)
    *outAffected = 0;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!EnterTableOp(table, errBuf, errBufLen))
    return EGDB_E_INVALIDARG;

  std::lock_guard<std::mutex> tableLock(table->mutex);

  EnumRows rows;
  egdb_result status = OpenRowEnumerator(table, utf8WhereClause, envelope,
                                         "the search for rows to delete failed",
                                         rows, errBuf, errBufLen);
  if (status != EGDB_OK)
    return status;

  Row       row;
  fgdbError nextHr = S_OK;
  int32_t   affected = 0;

  while ((nextHr = rows.Next(row)) == S_OK)
  {
    const fgdbError hr = table->table->Delete(row);
    if (hr != S_OK)
    {
      rows.Close();
      return SetLastError(errBuf, errBufLen, hr, "failed to delete a row");
    }
    ++affected;
  }

  rows.Close();

  if (nextHr != S_FALSE)
    return SetLastError(errBuf, errBufLen, nextHr, "failed while iterating the rows to delete");

  if (outAffected != NULL)
    *outAffected = affected;
  return EGDB_OK;
}

// ---------------------------------------------------------------------------
// Cursor metadata: length / nullability
// ---------------------------------------------------------------------------

EGDB_EXPORT int32_t egdb_cursor_field_length(const egdb_cursor* cursor, int32_t index)
{
  egdb_cursor* mutableCursor = const_cast<egdb_cursor*>(cursor);
  if (!IsLiveCursor(mutableCursor))
    return -2;

  std::lock_guard<std::mutex> lock(mutableCursor->mutex);
  if (index < 0 || static_cast<size_t>(index) >= mutableCursor->fieldLengths.size())
    return -2;

  return mutableCursor->fieldLengths[static_cast<size_t>(index)];
}

EGDB_EXPORT int32_t egdb_cursor_field_nullable(const egdb_cursor* cursor, int32_t index)
{
  egdb_cursor* mutableCursor = const_cast<egdb_cursor*>(cursor);
  if (!IsLiveCursor(mutableCursor))
    return -1;

  std::lock_guard<std::mutex> lock(mutableCursor->mutex);
  if (index < 0 || static_cast<size_t>(index) >= mutableCursor->fieldNullable.size())
    return -1;

  return mutableCursor->fieldNullable[static_cast<size_t>(index)];
}

EGDB_EXPORT const char* egdb_version(void)
{
  return kVersion;
}

} // extern "C"
