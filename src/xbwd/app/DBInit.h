#pragma once

#include <xbwd/basics/ChainTypes.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xbwd {
namespace db_init {

std::string const xChainSyncTable("XChainSync");

std::string const&
xChainDBName();

std::string const&
xChainTableName(ChainType chain);

std::string const&
xChainCreateAccountTableName(ChainType chain);

std::vector<std::string> const&
xChainDBPragma();

std::vector<std::string> const&
xChainDBInit();

}  // namespace db_init
}  // namespace xbwd
