// Copyright 2015 Andreas Schaefer
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
//

// test for Realm's element distribution code

#include "realm/realm.h"
#include <iostream>
#include <libgeodecomp/geometry/region.h>

void top_level_task(const void *args, size_t arglen, Realm::Processor p)
{
    Realm::ElementMask m1(1000000000, 0);
    m1.enable(0, 10);
    Realm::ElementMask m2(1000000000, 0);
    m2.enable(80000000, 50);

    double t_start = Realm::Clock::current_time();
    Realm::ElementMask m3 = m1 | m2;
    double t_end = Realm::Clock::current_time();
    double elapsed = t_end - t_start;
    std::cout << "elapsed: " << elapsed << "\n";

    t_start = Realm::Clock::current_time();
    assert(60 == m3.pop_count(true));
    t_end = Realm::Clock::current_time();
    elapsed = t_end - t_start;
    std::cout << "elapsed: " << elapsed << "\n";

    using LibGeoDecomp::Coord;
    using LibGeoDecomp::Region;
    using LibGeoDecomp::Streak;
    Region<1> r1;
    r1 << LibGeoDecomp::Streak<1>(Coord<1>(0), 10);
    Region<1> r2;
    r2 << LibGeoDecomp::Streak<1>(Coord<1>(80000000), 80000000 + 50);

    t_start = Realm::Clock::current_time();
    Region<1> r3 = r1 + r2;
    t_end = Realm::Clock::current_time();
    elapsed = t_end - t_start;
    std::cout << "elapsed: " << elapsed << "\n";

    t_start = Realm::Clock::current_time();
    assert(60 == r3.size());
    t_end = Realm::Clock::current_time();
    elapsed = t_end - t_start;
    std::cout << "elapsed: " << elapsed << "\n";

    Realm::Runtime::get_runtime().shutdown();
}

int main(int argc, char *argv[])
{
    Realm::Runtime rt;

    rt.init(&argc, &argv);
    rt.register_task(Realm::Processor::TASK_ID_FIRST_AVAILABLE, top_level_task);

    rt.run(Realm::Processor::TASK_ID_FIRST_AVAILABLE, Realm::Runtime::ONE_TASK_ONLY);

    return 0;
}
