//
// EsriGdbBridgeXml.cpp
//
// XML fragment normalisation and table definition validation.
// 
// The FileGDB API is picky about the XML handed to CreateTable / AddField /
// AddIndex / CreateDomain, so the bridge repairs common omissions and
// rejects definitions the Esri parser would choke on (or crash on).
//

#include "EsriGdbBridge.h"

#include <cstring>

namespace esrigdb_bridge {


// ---------------------------------------------------------------------------
// XML fragment normalization
//
// The FileGDB API is picky about the XML handed to CreateTable / AddField /
// AddIndex / CreateDomain: the root element must carry the esri: prefix and
// every prefix it uses must be declared. The samples ship such fragments
// (samples/XMLsamples/*.xml), but hand written XML usually does not, so the
// bridge repairs common omissions instead of failing deep inside libxml.
// ---------------------------------------------------------------------------
/// Locates the root element of `xml`. Returns false when xml has no element.
bool FindRootTag(const std::string& xml, XmlTag& tag)
{
  size_t open = xml.find('<');
  while (open != std::string::npos)
  {
    const char next = (open + 1 < xml.size()) ? xml[open + 1] : '\0';
    if (next != '?' && next != '!' && next != '/' && next != '\0')
      break;                                   // this is the root element
    open = xml.find('<', open + 1);
  }
  if (open == std::string::npos || open + 1 >= xml.size())
    return false;

  size_t nameEnd = open + 1;
  while (nameEnd < xml.size())
  {
    const char c = xml[nameEnd];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
      break;
    ++nameEnd;
  }
  const size_t tagEnd = xml.find('>', nameEnd);
  if (tagEnd == std::string::npos)
    return false;

  tag.nameStart = open + 1;
  tag.nameEnd   = nameEnd;
  tag.tagEnd    = tagEnd;
  tag.name      = xml.substr(tag.nameStart, tag.nameEnd - tag.nameStart);
  return !tag.name.empty();
}

/// Returns the part of an element name after its prefix ("esri:Field" -> "Field").
std::string LocalName(const std::string& name)
{
  const size_t colon = name.find(':');
  return (colon == std::string::npos) ? name : name.substr(colon + 1);
}

/// True when the start tag carries a namespace declaration for `decl`
/// (e.g. "xmlns:xsi"), honouring both ' and " quoting.
bool HasNamespaceDecl(const std::string& xml, size_t from, size_t length, const char* decl)
{
  const size_t end  = from + length;
  const size_t name = strlen(decl);

  size_t pos = xml.find(decl, from);
  while (pos != std::string::npos && pos + name <= end)
  {
    size_t after = pos + name;
    while (after < end &&
           (xml[after] == ' ' || xml[after] == '\t' || xml[after] == '\r' || xml[after] == '\n'))
      ++after;
    if (after < end && xml[after] == '=')
      return true;
    pos = xml.find(decl, pos + 1);
  }
  return false;
}

/// Rewrites the root element (and its closing tag) to `newName` when needed
/// and adds the esri/xsi/xs namespace declarations that are still missing.
/// @param[in] newName  NULL keeps the local name and only adds the esri prefix.
void NormalizeXmlFragment(std::string& xml, const char* newName)
{
  // A UTF-8 BOM or leading whitespace confuses the API's parser.
  if (xml.size() >= 3 &&
      static_cast<unsigned char>(xml[0]) == 0xEF &&
      static_cast<unsigned char>(xml[1]) == 0xBB &&
      static_cast<unsigned char>(xml[2]) == 0xBF)
    xml.erase(0, 3);
  const size_t firstChar = xml.find_first_not_of(" \t\r\n");
  if (firstChar == std::string::npos)
    return;
  if (firstChar > 0)
    xml.erase(0, firstChar);

  XmlTag tag;
  if (!FindRootTag(xml, tag))
    return;

  // Decide the name the root element should carry.
  std::string target = tag.name;
  if (newName != NULL && *newName != '\0')
    target = newName;
  else if (tag.name.find(':') == std::string::npos)
    target = "esri:" + tag.name;

  // Opening tag.
  if (target != tag.name)
    xml.replace(tag.nameStart, tag.nameEnd - tag.nameStart, target);

  // Closing tag (the last "</...>" of the document).
  const size_t close = xml.rfind('<');
  if (close != std::string::npos && close + 1 < xml.size() && xml[close + 1] == '/')
  {
    size_t closeNameEnd = close + 2;
    while (closeNameEnd < xml.size())
    {
      const char c = xml[closeNameEnd];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
        break;
      ++closeNameEnd;
    }
    if (close + 2 < closeNameEnd && xml.compare(close + 2, closeNameEnd - close - 2, target) != 0)
      xml.replace(close + 2, closeNameEnd - close - 2, target);
  }

  // Namespace declarations on the start tag.
  if (!FindRootTag(xml, tag))
    return;

  const size_t tagLength = tag.tagEnd - tag.nameStart;
  std::string  decls;

  if (!HasNamespaceDecl(xml, tag.nameStart, tagLength, "xmlns:xsi"))
    decls += " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"";
  if (!HasNamespaceDecl(xml, tag.nameStart, tagLength, "xmlns:xs"))
    decls += " xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"";
  if (!HasNamespaceDecl(xml, tag.nameStart, tagLength, "xmlns:esri"))
    decls += " xmlns:esri=\"http://www.esri.com/schemas/ArcGIS/10.1\"";

  if (!decls.empty())
  {
    size_t insertAt = tag.tagEnd;
    if (insertAt > tag.nameStart && xml[insertAt - 1] == '/')
      --insertAt;                        // self closing tag: insert before '/'
    xml.insert(insertAt, decls);
  }
}

// ---------------------------------------------------------------------------
// Table definition validation
//
// CreateTable hands the definition to an Esri parser that is not defensive:
// a definition without <CLSID> takes the process down with a segmentation
// fault, and one without <Indexes> (or with <Indexes> after <CLSID>) fails
// with an unhelpful "General function failure". Both are checked up front so
// callers get a precise argument error instead.
// ---------------------------------------------------------------------------
/// Collects the direct children of the root element.
void CollectRootChildren(const std::string& xml, std::vector<XmlChild>& children)
{
  children.clear();

  XmlTag root;
  if (!FindRootTag(xml, root))
    return;

  size_t pos = root.tagEnd + 1;
  int depth = 1;
  while (pos < xml.size())
  {
    const size_t open = xml.find('<', pos);
    if (open == std::string::npos)
      return;

    const char next = (open + 1 < xml.size()) ? xml[open + 1] : '\0';
    if (next == '?')                                    // XML declaration
    {
      const size_t gt = xml.find('>', open);
      if (gt == std::string::npos) return;
      pos = gt + 1;
      continue;
    }
    if (next == '!')                                    // comment / doctype
    {
      const size_t gt = xml.find('>', open);
      if (gt == std::string::npos) return;
      pos = gt + 1;
      continue;
    }
    if (next == '/')
    {
      const size_t gt = xml.find('>', open);
      if (gt == std::string::npos) return;
      if (--depth <= 0)                                 // root element closed
        return;
      pos = gt + 1;
      continue;
    }
    if (next == '\0')
      return;

    size_t nameEnd = open + 1;
    while (nameEnd < xml.size())
    {
      const char c = xml[nameEnd];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
        break;
      ++nameEnd;
    }
    const size_t tagEnd = xml.find('>', nameEnd);
    if (tagEnd == std::string::npos)
      return;

    const std::string name = xml.substr(open + 1, nameEnd - open - 1);
    const bool selfClosing = (tagEnd > open && xml[tagEnd - 1] == '/');

    if (depth == 1)
    {
      XmlChild child;
      child.name  = name;
      child.start = open;
      children.push_back(child);
    }
    if (!selfClosing)
      ++depth;
    pos = tagEnd + 1;
  }
}

/// True when `xml` is a table definition the FileGDB API can safely consume;
/// otherwise `problem` describes what has to change.
bool ValidateTableDefinition(const std::string& xml, std::string& problem)
{
  problem.clear();

  XmlTag root;
  if (!FindRootTag(xml, root))
  {
    problem = "the table definition does not contain an XML element";
    return false;
  }
  if (LocalName(root.name) != "DataElement")
  {
    problem = "the root element of the table definition must be <esri:DataElement>, not <" +
              root.name + ">";
    return false;
  }

  static const char* required[] = { "CatalogPath", "Name", "DatasetType",
                                    "HasOID", "Fields", "Indexes", "CLSID" };

  std::vector<XmlChild> children;
  CollectRootChildren(xml, children);
  if (children.empty())
  {
    problem = "the table definition has no child elements under <esri:DataElement>";
    return false;
  }

  size_t indexPos = std::string::npos;
  size_t clsidPos = std::string::npos;
  std::string missing;

  for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
  {
    bool found = false;
    for (size_t c = 0; c < children.size(); ++c)
    {
      if (children[c].name == required[i])
      {
        found = true;
        if (children[c].name == "Indexes") indexPos = children[c].start;
        if (children[c].name == "CLSID")   clsidPos = children[c].start;
        break;
      }
    }
    if (!found)
    {
      missing += missing.empty() ? "<" : ", <";
      missing += required[i];
      missing += ">";
    }
  }

  if (!missing.empty())
  {
    problem = "the table definition is missing the element(s) " + missing +
              " - a definition that is not a complete <esri:DataElement> cannot be created";
    return false;
  }

  if (indexPos > clsidPos)
  {
    problem = "<Indexes> must appear before <CLSID> in a table definition";
    return false;
  }

  return true;
}

} // namespace esrigdb_bridge
