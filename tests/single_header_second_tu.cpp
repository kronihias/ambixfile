// SPDX-License-Identifier: MIT
// Includes the single header without the implementation, as user code would.
#include "ambixfile.hpp"

bool secondTranslationUnitWorks() {
    return ambixfile::fumaToAmbix(1).rows() == 4 && ambixfile::orderToChannels(2) == 9;
}
