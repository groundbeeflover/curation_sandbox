# SPDX-License-Identifier: Apache-2.0
# The Qt shell is configured only with CS_BUILD_APP (the app preset), since it needs Qt 6.
if(NOT CS_BUILD_APP)
    return()
endif()

cs_add_module(NAME ui DEPS editor gpu assets core)
