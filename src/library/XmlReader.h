#pragma once

#include "util/Result.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace devildj::library
{

// A small XML reader for library exports (P2-09): elements, attributes, text, the five named entities and
// numeric character references, comments, CDATA, the XML declaration and a DOCTYPE (skipped). UTF-8 only
// (a BOM is skipped). No namespaces, no DTD processing, no external entities - nothing in the file can make the
// reader fetch anything.
struct XmlElement
{
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text; // the element's own text (child elements' text not included), entities decoded
    std::vector<std::unique_ptr<XmlElement>> children;

    const std::string* attribute(const std::string& key) const;
    std::string attributeOr(const std::string& key, const std::string& fallback = {}) const;
    const XmlElement* child(std::string_view name) const; // the first child with this name
    std::vector<const XmlElement*> childrenNamed(std::string_view name) const;
};

util::Result<std::unique_ptr<XmlElement>> parseXml(std::string_view text);

} // namespace devildj::library
