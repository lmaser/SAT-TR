#pragma once

// ============================================================================
// InfoContent.h — Structured content for the SAT-TR info popup.
// ============================================================================

namespace InfoContent
{
    static constexpr const char* version = "1.4";

    static constexpr const char* xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<info>
  <content>
    <heading>SAT-TR v1.4</heading>
    <spacer/>
    <text>by Nemester</text>
    <link url="https://github.com/lmaser/SAT-TR">Github Repository</link>
    <separator>&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;&#x2500;</separator>
    <link url="https://ko-fi.com/nemester">Support on Ko-fi</link>
  </content>
</info>
)xml";
}
