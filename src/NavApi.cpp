// NavSim's frame step with the frame profiler sections (the profiler is engine core; NavSim is in IYIEngineSim).

#include "IYI/NavSim.h"
#include "IYI/Profiler.h"
#include <algorithm>

namespace IYI::Api {

void NavStep(NavSim& n, double dt, std::optional<NavXY> focus)
{
    dt = std::min(dt, NavSim::MAX_DT);
    if (!n.Wait) {
        // Live: the game thread waits for the last step (usually done by now), shows it, reads the map's changes
        // and starts the next step on the nav thread. The counts are the last step's; worker_ms is its time there.
        ProfileBegin("routes", true);
        n.Join();
        n.Publish();
        ProfileCount("requests", n.ShownCount.Routes);
        ProfileCount("worker_ms", n.WorkerMs);
        n.StartStep(dt, focus);
        ProfileEnd(true);
        ProfileBegin("vehicles", true);
        ProfileCount("active", n.ShownCount.Vehicles);
        ProfileCount("dormant", static_cast<double>(n.ShownVehicles.Size()) - n.ShownCount.Vehicles);   // shown: the live ones are the nav thread's now
        ProfileEnd(true);
        ProfileBegin("walkers", true);
        ProfileCount("active", n.ShownCount.Walkers);
        ProfileCount("dormant", static_cast<double>(n.ShownWalkers.Size()) - n.ShownCount.Walkers);
        ProfileCount("near", n.ShownCount.Near);
        ProfileEnd(true);
        return;
    }
    const NavXY* f = focus ? &*focus : nullptr;
    ProfileBegin("routes", true);
    ProfileCount("requests", n.Count.Routes);
    n.Count.Routes = 0;
    n.Update(dt);
    ProfileEnd(true);
    ProfileBegin("vehicles", true);
    n.StepVehicles(dt, f);
    ProfileCount("active", n.Count.Vehicles);
    ProfileCount("dormant", static_cast<double>(n.Vehicles.Size()) - n.Count.Vehicles);
    ProfileEnd(true);
    ProfileBegin("walkers", true);
    n.StepWalkers(dt, f);
    ProfileCount("active", n.Count.Walkers);
    ProfileCount("dormant", static_cast<double>(n.Walkers.Size()) - n.Count.Walkers);
    ProfileCount("near", n.Count.Near);
    ProfileEnd(true);
    ProfileBegin("air", true);
    n.StepAir(dt);
    ProfileEnd(true);
    n.Publish();
}

} // namespace IYI::Api
