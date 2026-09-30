//
// EsriGdbBridgeUtil.cpp
//
// Encoding, error reporting, value conversion, catalog paths and the
// plain C marshalling helpers used by the public ABI.
//

#include "EsriGdbBridge.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace esrigdb_bridge {


const char* const kVersion = "1.0.0";

// ---------------------------------------------------------------------------
// UTF-8 <-> wide string helpers
//
// The Esri API takes and returns std::wstring. wchar_t is UTF-32 on
// Linux/macOS but UTF-16 on Windows, so both directions have to handle
// surrogate pairs when the target is 16 bit. The branches are written as
// plain runtime tests on sizeof(wchar_t) - the compiler folds them away on
// the other platform, so no #ifdef dance is needed.
// ---------------------------------------------------------------------------

// Appends one Unicode scalar value as one or two wchar_t units.
static void AppendWide(std::wstring& out, unsigned int cp)
{
  if (sizeof(wchar_t) == 2)
  {
    if (cp >= 0x10000)
    {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));   // high surrogate
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));  // low surrogate
      return;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
      return;                       // stray surrogate half in the input
  }
  else if (cp > 0x10FFFF)
  {
    cp = 0xFFFD;                    // not a Unicode scalar value
  }
  out.push_back(static_cast<wchar_t>(cp));
}

std::wstring Utf8ToWide(const char* utf8)
{
  std::wstring out;
  if (utf8 == NULL)
    return out;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != 0)
  {
    unsigned int  cp   = 0;
    int           len  = 0;
    unsigned char lead = *p;

    if (lead < 0x80)             { cp = lead;                len = 1; }
    else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F;      len = 2; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F;      len = 3; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07;      len = 4; }
    else                        { ++p; continue; }               // skip invalid byte

    for (int i = 1; i < len; ++i)
    {
      if ((p[i] & 0xC0) != 0x80) { len = 0; break; }
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (len == 0) { ++p; continue; }

    AppendWide(out, cp);
    p += len;
  }
  return out;
}

void AppendUtf8(std::string& out, unsigned int cp)
{
  if (cp < 0x80)
    out.push_back(static_cast<char>(cp));
  else if (cp < 0x800)
  {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000)
  {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string WideToUtf8(const std::wstring& wide)
{
  std::string out;
  out.reserve(wide.size() * 3);

  for (size_t i = 0; i < wide.size(); ++i)
  {
    unsigned int cp = static_cast<unsigned int>(wide[i]);

    if (sizeof(wchar_t) == 2)
    {
      // UTF-16: stitch surrogate pairs back into one scalar value.
      cp &= 0xFFFFu;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size())
      {
        unsigned int low = static_cast<unsigned int>(wide[i + 1]) & 0xFFFFu;
        if (low >= 0xDC00 && low <= 0xDFFF)
        {
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
          ++i;
        }
        else
        {
          cp = 0xFFFDu;             // unpaired high surrogate
        }
      }
      else if (cp >= 0xD800 && cp <= 0xDFFF)
      {
        cp = 0xFFFDu;               // unpaired low surrogate
      }
    }
    else if (cp > 0x10FFFFu)
    {
      cp = 0xFFFDu;                 // incl. sign-extended negative wchar_t
    }

    AppendUtf8(out, cp);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Base64 (used for blob / geometry payloads)
// ---------------------------------------------------------------------------
std::string Base64Encode(const unsigned char* data, size_t len)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3)
  {
    unsigned int v = static_cast<unsigned int>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<unsigned int>(data[i + 1]) << 8;
    if (i + 2 < len) v |= static_cast<unsigned int>(data[i + 2]);

    out.push_back(alphabet[(v >> 18) & 0x3F]);
    out.push_back(alphabet[(v >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? alphabet[(v >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? alphabet[v & 0x3F] : '=');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
egdb_result SetLastError(char* errBuf, int32_t errBufLen, fgdbError hr, const char* context)
{
  if (errBuf != NULL && errBufLen > 0)
  {
    std::string msg = (context != NULL) ? context : "operation failed";

    if (hr != S_OK)
    {
      std::wstring description;
      ErrorInfo::GetErrorDescription(hr, description);
      if (!description.empty())
      {
        msg += ": ";
        msg += WideToUtf8(description);
      }
    }

    std::ostringstream os;
    os << msg << " [fgdbError=" << static_cast<int>(hr)
       << " (0x" << std::hex << static_cast<unsigned int>(hr) << ")]";

    std::string text = os.str();
    size_t      copy = (text.size() < static_cast<size_t>(errBufLen - 1))
                       ? text.size() : static_cast<size_t>(errBufLen - 1);
    memcpy(errBuf, text.data(), copy);
    errBuf[copy] = '\0';
  }
  return static_cast<egdb_result>(hr);
}

egdb_result SetNativeError(char* errBuf, int32_t errBufLen, egdb_result code, const char* context)
{
  if (errBuf != NULL && errBufLen > 0)
  {
    const char* msg = (context != NULL) ? context : "operation failed";
    size_t      copy = strlen(msg);
    if (copy >= static_cast<size_t>(errBufLen))
      copy = static_cast<size_t>(errBufLen - 1);
    memcpy(errBuf, msg, copy);
    errBuf[copy] = '\0';
  }
  return code;
}

egdb_result SetArgumentError(char* errBuf, int32_t errBufLen, const char* context)
{
  return SetNativeError(errBuf, errBufLen, EGDB_E_INVALIDARG, context);
}

char* DupString(const std::string& value)
{
  char* copy = static_cast<char*>(malloc(value.size() + 1));
  if (copy == NULL)
    return NULL;
  memcpy(copy, value.data(), value.size());
  copy[value.size()] = '\0';
  return copy;
}

/// Allocates a char* array holding (ownership of) the pointers of `list`.
char** DupPointerArray(const std::vector<char*>& list)
{
  if (list.empty())
    return NULL;
  char** array = static_cast<char**>(malloc(sizeof(char*) * list.size()));
  if (array == NULL)
    return NULL;
  for (size_t i = 0; i < list.size(); ++i)
    array[i] = list[i];
  return array;
}

int32_t* DupIntArray(const std::vector<int32_t>& list)
{
  if (list.empty())
    return NULL;
  int32_t* array = static_cast<int32_t*>(malloc(sizeof(int32_t) * list.size()));
  if (array == NULL)
    return NULL;
  memcpy(array, list.data(), sizeof(int32_t) * list.size());
  return array;
}

void FreeStrings(std::vector<char*>& list)
{
  for (size_t i = 0; i < list.size(); ++i)
    free(list[i]);
  list.clear();
}

// ---------------------------------------------------------------------------
// Value extraction: converts a row cell to a UTF-8 string
// ---------------------------------------------------------------------------
bool FormatNumber(double value, int significantDigits, std::string& out)
{
  std::ostringstream os;
  os.precision(significantDigits);
  os << value;
  out = os.str();
  return true;
}

/// Extracts the value of the given field as UTF-8 text.
/// Returns false when the value could not be read (read error).
bool ExtractValue(Row& row, int fieldNumber, FieldType fieldType,
                  std::string& out, bool& isNull)
{
  out.clear();
  isNull = false;

  if (row.IsNull(fieldNumber, isNull) != S_OK)
    return false;
  if (isNull)
    return true;                                    // out stays empty, caller marks NULL

  fgdbError hr = S_OK;

  switch (fieldType)
  {
    case fieldTypeSmallInteger:
    {
      short value = 0;
      if ((hr = row.GetShort(fieldNumber, value)) != S_OK)
        return false;
      std::ostringstream os;
      os << value;
      out = os.str();
      break;
    }

    case fieldTypeInteger:
    case fieldTypeOID:
    {
      int32 value = 0;
      if (fieldType == fieldTypeOID)
      {
        if ((hr = row.GetOID(value)) != S_OK)
          return false;
      }
      else if ((hr = row.GetInteger(fieldNumber, value)) != S_OK)
        return false;
      std::ostringstream os;
      os << value;
      out = os.str();
      break;
    }

    case fieldTypeSingle:
    {
      float value = 0.0f;
      if ((hr = row.GetFloat(fieldNumber, value)) != S_OK)
        return false;
      FormatNumber(static_cast<double>(value), 9, out);   // max_digits10 for float
      break;
    }

    case fieldTypeDouble:
    {
      double value = 0.0;
      if ((hr = row.GetDouble(fieldNumber, value)) != S_OK)
        return false;
      FormatNumber(value, 17, out);                       // max_digits10 for double
      break;
    }

    case fieldTypeString:
    case fieldTypeXML:
    {
      if (fieldType == fieldTypeXML)
      {
        std::string value;
        if ((hr = row.GetXML(fieldNumber, value)) != S_OK)
          return false;
        out = value;
      }
      else
      {
        std::wstring value;
        if ((hr = row.GetString(fieldNumber, value)) != S_OK)
          return false;
        out = WideToUtf8(value);
      }
      break;
    }

    case fieldTypeDate:
    {
      tm dateTime;
      memset(&dateTime, 0, sizeof(dateTime));
      if ((hr = row.GetDate(fieldNumber, dateTime)) != S_OK)
        return false;
      char buffer[64];
      snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d",
               dateTime.tm_year + 1900, dateTime.tm_mon + 1, dateTime.tm_mday,
               dateTime.tm_hour, dateTime.tm_min, dateTime.tm_sec);
      out = buffer;
      break;
    }

    case fieldTypeGUID:
    case fieldTypeGlobalID:
    {
      Guid guid;
      if (fieldType == fieldTypeGUID)
      {
        if ((hr = row.GetGUID(fieldNumber, guid)) != S_OK)
          return false;
      }
      else if ((hr = row.GetGlobalID(guid)) != S_OK)
        return false;

      std::wstring text;
      if (guid.ToString(text) != S_OK)
        return false;
      out = WideToUtf8(text);
      break;
    }

    case fieldTypeBlob:
    {
      ByteArray bytes;
      if ((hr = row.GetBinary(fieldNumber, bytes)) != S_OK)
        return false;
      out = Base64Encode(bytes.byteArray, bytes.inUseLength);
      break;
    }

    case fieldTypeGeometry:
    {
      ShapeBuffer shape;
      if ((hr = row.GetGeometry(shape)) != S_OK)
        return false;
      if (shape.IsEmpty() || shape.shapeBuffer == NULL)
      {
        isNull = true;
        break;
      }
      out = Base64Encode(shape.shapeBuffer, shape.inUseLength);
      break;
    }

    case fieldTypeRaster:
    default:
      isNull = true;                                  // not serialised by this bridge
      break;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Base64 decoding (Blob / Geometry values arrive base64 encoded)
// ---------------------------------------------------------------------------
inline int Base64Value(unsigned char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+')             return 62;
  if (c == '/')             return 63;
  return -1;
}

bool Base64Decode(const char* text, std::vector<unsigned char>& out)
{
  out.clear();
  if (text == NULL)
    return false;

  int val  = 0;
  int valb = -8;

  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p)
  {
    if (*p == '=')
      break;
    const int d = Base64Value(*p);
    if (d < 0)
    {
      if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        continue;
      return false;                     // not valid base64
    }
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0)
    {
      out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Text -> typed value parsing (for insert / update)
// ---------------------------------------------------------------------------
bool ParseInt16(const char* text, short& out)
{
  char*  end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE ||
      value < -32768L || value > 32767L)
    return false;
  out = static_cast<short>(value);
  return true;
}

bool ParseInt32(const char* text, int32& out)
{
  char*  end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE ||
      value < -2147483648L || value > 2147483647L)
    return false;
  out = static_cast<int32>(value);
  return true;
}

bool ParseFloat(const char* text, float& out)
{
  char*  end = NULL;
  errno = 0;
  const float value = strtof(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE)
    return false;
  out = value;
  return true;
}

bool ParseDouble(const char* text, double& out)
{
  char*  end = NULL;
  errno = 0;
  const double value = strtod(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE)
    return false;
  out = value;
  return true;
}

/// Parses "yyyy-MM-ddTHH:mm:ss" (a leading date only form is also accepted).
bool ParseDate(const char* text, tm& out)
{
  memset(&out, 0, sizeof(out));

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  const int parsed = sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d",
                            &year, &month, &day, &hour, &minute, &second);
  if (parsed < 3)
    return false;
  if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
    return false;

  out.tm_year  = year - 1900;
  out.tm_mon   = month - 1;
  out.tm_mday  = day;
  out.tm_hour  = hour;
  out.tm_min   = minute;
  out.tm_sec   = second;
  out.tm_isdst = -1;
  return true;
}

/// Number of Unicode characters (UTF-8 code points) in a UTF-8 string.
size_t Utf8CharCount(const char* text)
{
  size_t count = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p)
  {
    if ((*p & 0xC0) != 0x80)          // not a continuation byte
      ++count;
  }
  return count;
}

const char* FieldTypeName(FieldType fieldType)
{
  switch (fieldType)
  {
    case fieldTypeSmallInteger: return "SmallInteger";
    case fieldTypeInteger:      return "Integer";
    case fieldTypeSingle:       return "Single";
    case fieldTypeDouble:       return "Double";
    case fieldTypeString:       return "String";
    case fieldTypeDate:         return "Date";
    case fieldTypeOID:          return "OID";
    case fieldTypeGeometry:     return "Geometry";
    case fieldTypeBlob:         return "Blob";
    case fieldTypeRaster:       return "Raster";
    case fieldTypeGUID:         return "GUID";
    case fieldTypeXML:          return "XML";
    case fieldTypeGlobalID:     return "GlobalID";
    default:                    return "Unknown";
  }
}

/// Converts a UTF-8 text value to `fieldType` and stores it in the row.
/// `utf8Value` equal to NULL means SQL NULL.
/// Returns false when the value cannot be converted or the write fails; on
/// failure `errorText` receives a description and `hr` the fgdbError (S_OK for
/// pure conversion problems).
bool ApplyValue(Row& row, const std::wstring& fieldName, FieldType fieldType,
                const char* utf8Value, fgdbError& hr, std::string& errorText)
{
  hr = S_OK;
  errorText.clear();

  const std::string fieldName8 = WideToUtf8(fieldName);

  if (utf8Value == NULL)
  {
    hr = row.SetNull(fieldName);
    if (hr != S_OK)
      errorText = "failed to set field '" + fieldName8 + "' to NULL";
    return hr == S_OK;
  }

  switch (fieldType)
  {
    case fieldTypeSmallInteger:
    {
      short value = 0;
      if (!ParseInt16(utf8Value, value))
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid SmallInteger for field '"
                  + fieldName8 + "'";
        return false;
      }
      hr = row.SetShort(fieldName, value);
      break;
    }

    case fieldTypeInteger:
    case fieldTypeOID:
    {
      int32 value = 0;
      if (!ParseInt32(utf8Value, value))
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid Integer for field '"
                  + fieldName8 + "'";
        return false;
      }
      hr = row.SetInteger(fieldName, value);
      break;
    }

    case fieldTypeSingle:
    {
      float value = 0.0f;
      if (!ParseFloat(utf8Value, value))
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid Single for field '"
                  + fieldName8 + "'";
        return false;
      }
      hr = row.SetFloat(fieldName, value);
      break;
    }

    case fieldTypeDouble:
    {
      double value = 0.0;
      if (!ParseDouble(utf8Value, value))
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid Double for field '"
                  + fieldName8 + "'";
        return false;
      }
      hr = row.SetDouble(fieldName, value);
      break;
    }

    case fieldTypeString:
      hr = row.SetString(fieldName, Utf8ToWide(utf8Value));
      break;

    case fieldTypeXML:
      hr = row.SetXML(fieldName, std::string(utf8Value));
      break;

    case fieldTypeDate:
    {
      tm dateTime;
      if (!ParseDate(utf8Value, dateTime))
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid Date for field '"
                  + fieldName8 + "' (expected yyyy-MM-ddTHH:mm:ss)";
        return false;
      }
      hr = row.SetDate(fieldName, dateTime);
      break;
    }

    case fieldTypeGUID:
    case fieldTypeGlobalID:
    {
      Guid guid;
      if (guid.FromString(Utf8ToWide(utf8Value)) != S_OK)
      {
        errorText = "value '" + std::string(utf8Value) + "' is not a valid GUID for field '"
                  + fieldName8 + "'";
        return false;
      }
      hr = row.SetGUID(fieldName, guid);
      break;
    }

    case fieldTypeBlob:
    {
      std::vector<unsigned char> bytes;
      if (!Base64Decode(utf8Value, bytes))
      {
        errorText = "field '" + fieldName8 + "' requires a base64 encoded Blob value";
        return false;
      }
      ByteArray buffer;
      if (!buffer.Allocate(bytes.size()))
      {
        errorText = "out of memory decoding the Blob value of field '" + fieldName8 + "'";
        return false;
      }
      if (!bytes.empty())
        memcpy(buffer.byteArray, bytes.data(), bytes.size());
      buffer.inUseLength = bytes.size();
      hr = row.SetBinary(fieldName, buffer);
      break;
    }

    case fieldTypeGeometry:
    {
      std::vector<unsigned char> bytes;
      if (!Base64Decode(utf8Value, bytes))
      {
        errorText = "the geometry value must be base64 encoded shape buffer bytes";
        return false;
      }
      ShapeBuffer shape;
      if (!shape.Allocate(bytes.size()))
      {
        errorText = "out of memory decoding the geometry value";
        return false;
      }
      if (!bytes.empty())
        memcpy(shape.shapeBuffer, bytes.data(), bytes.size());
      shape.inUseLength = bytes.size();
      hr = row.SetGeometry(shape);
      break;
    }

    case fieldTypeRaster:
    default:
      errorText = "field '" + fieldName8 + "' has unsupported type "
                + FieldTypeName(fieldType);
      return false;
  }

  if (hr != S_OK)
  {
    errorText = "failed to write field '" + fieldName8 + "'";
    return false;
  }
  return true;
}

/// Dataset paths in the catalog always start with a backslash, e.g. "\Cities".
std::wstring RootedPath(const char* utf8Path)
{
  std::wstring path = Utf8ToWide(utf8Path != NULL ? utf8Path : "");
  if (path.empty() || path[0] != L'\\')
    path.insert(path.begin(), L'\\');
  return path;
}

// ---------------------------------------------------------------------------
// Envelope / string array helpers
// ---------------------------------------------------------------------------

/// Case insensitive ASCII comparison (FileGDB field names are not case sensitive).
bool EqualsIgnoreCase(const std::string& a, const char* b)
{
  if (b == NULL)
    return false;
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i)
  {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb)
      return false;
  }
  return i == a.size() && b[i] == '\0';
}

/// Index of the field with the given UTF-8 name, or -1 when unknown.
int FindFieldIndex(const egdb_tableobj* tableObj, const char* utf8Name)
{
  if (utf8Name == NULL)
    return -1;
  for (size_t i = 0; i < tableObj->fieldNames.size(); ++i)
  {
    if (EqualsIgnoreCase(tableObj->fieldNames[i], utf8Name))
      return static_cast<int>(i);
  }
  return -1;
}

Envelope ToEnvelope(const egdb_envelope* envelope)
{
  Envelope result;
  if (envelope != NULL && envelope->use_envelope != 0)
  {
    result.xMin = envelope->xmin;
    result.yMin = envelope->ymin;
    result.xMax = envelope->xmax;
    result.yMax = envelope->ymax;
  }
  return result;
}

bool HasEnvelope(const egdb_envelope* envelope)
{
  return envelope != NULL && envelope->use_envelope != 0;
}

/// Moves UTF-8 strings into an egdb_string_array (the array owns them).
void FillStringArray(const std::vector<std::string>& items, egdb_string_array* out)
{
  out->count = static_cast<int32_t>(items.size());
  out->items = NULL;
  if (items.empty())
    return;

  out->items = static_cast<char**>(calloc(items.size(), sizeof(char*)));
  if (out->items == NULL)
  {
    out->count = 0;
    return;
  }
  for (size_t i = 0; i < items.size(); ++i)
    out->items[i] = DupString(items[i]);
}

void InitTable(egdb_table* table)
{
  table->field_count = 0;
  table->row_count   = 0;
  table->field_names = NULL;
  table->field_types = NULL;
  table->values      = NULL;
}


} // namespace esrigdb_bridge
