// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace lshaz {

// Text of src/hypothesis/templates/pmu_calib.h, embedded at configure time and
// emitted verbatim into every experiment bundle as src/pmu_calib.h. The
// template is also compiled directly by pipeline_unit_test, so the code under
// test and the code shipped in a bundle cannot drift.
const char *pmuCalibrationTemplate();

} // namespace lshaz
