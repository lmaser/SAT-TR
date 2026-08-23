#pragma once

#include "../../../TR-Shared/LoaderUIV2/Contract/LoaderPluginDefinition.h"

namespace SATTR::UIV2
{
static_assert(TR::LoaderUIV2::TR_LOADER_UI_V2_API_VERSION == 0x00020407u,
              "SAT-TR requires LoaderUIV2 API 2.4.7");
inline constexpr std::string_view SAT_REQUIRED_LOADER_UI_V2_REVISION =
    "loader-ui-v2-semantic-routes-20260804";
static_assert(SAT_REQUIRED_LOADER_UI_V2_REVISION == TR::LoaderUIV2::TR_LOADER_UI_V2_REVISION,
              "SAT-TR and LoaderUIV2 revisions are not coordinated");

const TR::LoaderUIV2::LoaderPluginDefinition& definition();
}
