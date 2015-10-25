// Copyright (c) 2015, Cloudera, inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef KUDU_UTIL_INIT_H
#define KUDU_UTIL_INIT_H

#include "kudu/gutil/macros.h"
#include "kudu/util/status.h"

namespace kudu {

// Return a NotSupported Status if the current CPU does not support the CPU flags
// required for Kudu.
Status CheckCPUFlags();

// Initialize Kudu, checking that the platform we are running on is supported, etc.
// Issues a FATAL log message if we fail to init.
void InitKuduOrDie();

} // namespace kudu
#endif /* KUDU_UTIL_INIT_H */
