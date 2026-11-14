# Copyright (c) 2026 The vycor-cpp Authors
# Original author: Alex Mason
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Lightweight Starlark build settings for @vycor//bazel flags (zero external skylib dependency)."""

VycorStringFlagInfo = provider(
    doc = "Carries the resolved string value of a @vycor//bazel build setting.",
    fields = {"value": "Resolved string flag value."},
)

VycorStringListFlagInfo = provider(
    doc = "Carries the resolved list[str] value of a @vycor//bazel build setting.",
    fields = {"values": "Resolved list of string flag values."},
)

def _vycor_string_flag_impl(ctx):
    return [VycorStringFlagInfo(value = ctx.build_setting_value)]

vycor_string_flag = rule(
    implementation = _vycor_string_flag_impl,
    build_setting = config.string(flag = True),
    doc = "Command-line configurable string flag for vycor-cpp rules and aspects.",
)

def _vycor_string_list_flag_impl(ctx):
    # Accept both repeatable/list values and comma-separated convenience strings.
    out = []
    for item in ctx.build_setting_value:
        for part in item.split(","):
            trimmed = part.strip()
            if trimmed:
                out.append(trimmed)
    return [VycorStringListFlagInfo(values = out)]

vycor_string_list_flag = rule(
    implementation = _vycor_string_list_flag_impl,
    build_setting = config.string_list(flag = True),
    doc = "Command-line configurable string-list flag for vycor-cpp rules and aspects.",
)
