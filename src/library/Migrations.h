#pragma once

#include "util/Result.h"

#include <span>
#include <string_view>

namespace devildj::library
{

class Connection;

// Schema migrations (ARCHITECTURE §7): an ordered list of SQL scripts. Script i brings the database from
// version i to i + 1; PRAGMA user_version stores the applied version. Never edit a released script: add a new
// one at the end.
std::span<const std::string_view> migrations() noexcept;
int latestSchemaVersion() noexcept;

// Applies every missing migration, each in its own transaction together with its user_version bump, so a
// failure leaves the database at the last good version. Returns the version afterwards. Fails when the file
// was written by a newer Devil-DJ (its version is above latestSchemaVersion()).
util::Result<int> migrate(Connection& db);

int schemaVersion(Connection& db);

} // namespace devildj::library
