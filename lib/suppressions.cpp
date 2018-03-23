/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2017 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "suppressions.h"

#include "path.h"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>   // std::isdigit, std::isalnum, etc
#include <stack>
#include <sstream>
#include <utility>

std::string Suppressions::parseFile(std::istream &istr)
{
    // Change '\r' to '\n' in the istr
    std::string filedata;
    std::string line;
    while (std::getline(istr, line))
        filedata += line + "\n";
    std::replace(filedata.begin(), filedata.end(), '\r', '\n');

    // Parse filedata..
    std::istringstream istr2(filedata);
    while (std::getline(istr2, line)) {
        // Skip empty lines
        if (line.empty())
            continue;

        // Skip comments
        if (line.length() >= 2 && line[0] == '/' && line[1] == '/')
            continue;

        const std::string errmsg(addSuppressionLine(line));
        if (!errmsg.empty())
            return errmsg;
    }

    return "";
}


std::string Suppressions::parseXmlFile(const char *filename)
{
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError error = doc.LoadFile(filename);
    if (error == tinyxml2::XML_ERROR_FILE_NOT_FOUND)
        return "File not found";

    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    for (const tinyxml2::XMLElement * e = rootnode->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (std::strcmp(e->Name(), "suppress")) {
            Suppression s;
            for (const tinyxml2::XMLElement * e2 = e->FirstChildElement(); e2; e2 = e2->NextSiblingElement()) {
                const char *text = e2->GetText() ? e2->GetText() : "";
                if (std::strcmp(e2->Name(), "id") == 0)
                    s.errorId = text;
                else if (std::strcmp(e2->Name(), "fileName") == 0)
                    s.fileName = text;
                else if (std::strcmp(e2->Name(), "lineNumber") == 0)
                    s.lineNumber = std::atoi(text);
                else if (std::strcmp(e2->Name(), "symbolName") == 0)
                    s.symbolName = text;
            }
            addSuppression(s);
        }
    }

    return "";
}

std::string Suppressions::addSuppressionLine(const std::string &line)
{
    std::istringstream lineStream(line);
    Suppressions::Suppression suppression;
    if (std::getline(lineStream, suppression.errorId, ':')) {
        if (std::getline(lineStream, suppression.fileName)) {
            // If there is not a dot after the last colon in "file" then
            // the colon is a separator and the contents after the colon
            // is a line number..

            // Get position of last colon
            const std::string::size_type pos = suppression.fileName.rfind(':');

            // if a colon is found and there is no dot after it..
            if (pos != std::string::npos &&
                suppression.fileName.find('.', pos) == std::string::npos) {
                // Try to parse out the line number
                try {
                    std::istringstream istr1(suppression.fileName.substr(pos+1));
                    istr1 >> suppression.lineNumber;
                } catch (...) {
                    suppression.lineNumber = 0;
                }

                if (suppression.lineNumber > 0) {
                    suppression.fileName.erase(pos);
                }
            }
        }
    }

    suppression.fileName = Path::fromNativeSeparators(suppression.fileName);

    return addSuppression(suppression);
}

std::string Suppressions::addSuppression(const Suppressions::Suppression &suppression)
{
    // Check that errorId is valid..
    if (suppression.errorId.empty()) {
        return "Failed to add suppression. No id.";
    }
    if (suppression.errorId != "*") {
        for (std::string::size_type pos = 0; pos < suppression.errorId.length(); ++pos) {
            if (suppression.errorId[pos] < 0 || (!std::isalnum(suppression.errorId[pos]) && suppression.errorId[pos] != '_')) {
                return "Failed to add suppression. Invalid id \"" + suppression.errorId + "\"";
            }
            if (pos == 0 && std::isdigit(suppression.errorId[pos])) {
                return "Failed to add suppression. Invalid id \"" + suppression.errorId + "\"";
            }
        }
    }

    _suppressions.push_back(suppression);

    return "";
}

static bool matchglob(const std::string &pattern, const std::string &name)
{
    const char *p = pattern.c_str();
    const char *n = name.c_str();
    std::stack<std::pair<const char *, const char *> > backtrack;

    for (;;) {
        bool matching = true;
        while (*p != '\0' && matching) {
            switch (*p) {
            case '*':
                // Step forward until we match the next character after *
                while (*n != '\0' && *n != p[1]) {
                    n++;
                }
                if (*n != '\0') {
                    // If this isn't the last possibility, save it for later
                    backtrack.push(std::make_pair(p, n));
                }
                break;
            case '?':
                // Any character matches unless we're at the end of the name
                if (*n != '\0') {
                    n++;
                } else {
                    matching = false;
                }
                break;
            default:
                // Non-wildcard characters match literally
                if (*n == *p) {
                    n++;
                } else {
                    matching = false;
                }
                break;
            }
            p++;
        }

        // If we haven't failed matching and we've reached the end of the name, then success
        if (matching && *n == '\0') {
            return true;
        }

        // If there are no other paths to try, then fail
        if (backtrack.empty()) {
            return false;
        }

        // Restore pointers from backtrack stack
        p = backtrack.top().first;
        n = backtrack.top().second;
        backtrack.pop();

        // Advance name pointer by one because the current position didn't work
        n++;
    }
}

bool Suppressions::Suppression::isMatch(const Suppressions::ErrorMessage &errmsg)
{
    if (!errorId.empty() && errorId != errmsg.errorId)
        return false;
    if (!fileName.empty() && fileName != errmsg.fileName)
        return false;
    if (lineNumber > 0 && lineNumber != errmsg.lineNumber)
        return false;
    if (!symbolName.empty() && errmsg.symbolNames.find(symbolName + '\n') == std::string::npos)
        return false;
    matched = true;
    return true;
}

bool Suppressions::isSuppressed(const Suppressions::ErrorMessage &errmsg)
{
    // TODO a set does not work well maybe
    for (Suppression &s : _suppressions) {
        if (s.isMatch(errmsg))
            return true;
    }
    return false;
}

bool Suppressions::isSuppressedLocal(const Suppressions::ErrorMessage &errmsg)
{
    // TODO a set does not work well maybe
    for (Suppression &s : _suppressions) {
        if (s.isMatch(errmsg))
            return true;
    }
    return false;
}

std::list<Suppressions::Suppression> Suppressions::getUnmatchedLocalSuppressions(const std::string &/*file*/, const bool /*unusedFunctionChecking*/) const
{
    std::list<Suppression> result;

    /*
    for (std::map<std::string, FileMatcher>::const_iterator i = _suppressions.begin(); i != _suppressions.end(); ++i) {
        if (!unusedFunctionChecking && i->first == "unusedFunction")
            continue;

        std::map<std::string, std::map<unsigned int, bool> >::const_iterator f = i->second._files.find(Path::fromNativeSeparators(file));
        if (f != i->second._files.end()) {
            for (std::map<unsigned int, bool>::const_iterator l = f->second.begin(); l != f->second.end(); ++l) {
                if (!l->second) {
                    result.push_back(SuppressionEntry(i->first, f->first, l->first));
                }
            }
        }
    }
    */
    return result;
}

std::list<Suppressions::Suppression> Suppressions::getUnmatchedGlobalSuppressions(const bool /*unusedFunctionChecking*/) const
{
    std::list<Suppression> result;
    /*
    for (std::map<std::string, FileMatcher>::const_iterator i = _suppressions.begin(); i != _suppressions.end(); ++i) {
        if (!unusedFunctionChecking && i->first == "unusedFunction")
            continue;

        // global suppressions..
        for (std::map<std::string, std::map<unsigned int, bool> >::const_iterator g = i->second._globs.begin(); g != i->second._globs.end(); ++g) {
            for (std::map<unsigned int, bool>::const_iterator l = g->second.begin(); l != g->second.end(); ++l) {
                if (!l->second) {
                    result.push_back(SuppressionEntry(i->first, g->first, l->first));
                }
            }
        }
    }
    */
    return result;
}
