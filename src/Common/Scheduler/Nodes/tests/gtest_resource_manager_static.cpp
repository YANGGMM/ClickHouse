#include <gtest/gtest.h>

#include <Common/Scheduler/Nodes/tests/ResourceTest.h>

#include <Common/Scheduler/Nodes/StaticResourceManager.h>
#include <Poco/Util/XMLConfiguration.h>

using namespace DB;

using ResourceTest = ResourceTestManager<StaticResourceManager>;
using TestGuard = ResourceTest::Guard;

TEST(SchedulerStaticResourceManager, Smoke)
{
    ResourceTest t;

    t.update(R"CONFIG(
        <clickhouse>
            <resources>
                <res1>
                    <node path="/">      <type>inflight_limit</type><max_requests>10</max_requests></node>
                    <node path="/prio">  <type>priority</type></node>
                    <node path="/prio/A"></node>
                    <node path="/prio/B"><priority>1</priority></node>
                </res1>
            </resources>
            <workload_classifiers>
                <A><res1>/prio/A</res1></A>
                <B><res1>/prio/B</res1></B>
            </workload_classifiers>
        </clickhouse>
    )CONFIG");

    ClassifierPtr ca = t.manager->acquire("A");
    ClassifierPtr cb = t.manager->acquire("B");

    for (int i = 0; i < 10; i++)
    {
        ResourceGuard ga(ca->get("res1"));
        ga.unlock();
        ResourceGuard gb(cb->get("res1"));
    }
}

TEST(SchedulerStaticResourceManager, Prioritization)
{
    std::optional<Priority> last_priority;
    auto check = [&] (Priority priority)
    {
        // Lock is not required here because this is called during request execution and we have max_requests = 1
        if (last_priority)
            EXPECT_TRUE(priority >= *last_priority); // Should be true if every queue arrived at the same time at busy period start
        last_priority = priority;
    };

    constexpr size_t threads_per_queue = 2;
    int requests_per_thead = 100;
    ResourceTest t(4 * threads_per_queue + 1);

    t.update(R"CONFIG(
        <clickhouse>
            <resources>
                <res1>
                    <node path="/">           <type>inflight_limit</type><max_requests>1</max_requests></node>
                    <node path="/prio">       <type>priority</type></node>
                    <node path="/prio/A">     <priority>1</priority></node>
                    <node path="/prio/B">     <priority>-1</priority></node>
                    <node path="/prio/C">     </node>
                    <node path="/prio/D">     </node>
                    <node path="/prio/leader"></node>
                </res1>
            </resources>
            <workload_classifiers>
                <A><res1>/prio/A</res1></A>
                <B><res1>/prio/B</res1></B>
                <C><res1>/prio/C</res1></C>
                <D><res1>/prio/D</res1></D>
                <leader><res1>/prio/leader</res1></leader>
            </workload_classifiers>
        </clickhouse>
    )CONFIG");

    for (String name : {"A", "B", "C", "D"})
    {
        for (int thr = 0; thr < threads_per_queue; thr++)
        {
            t.threads.emplace_back([&, name]
            {
                ClassifierPtr c = t.manager->acquire(name);
                ResourceLink link = c->get("res1");
                t.startBusyPeriod(link, 1, requests_per_thead);
                for (int req = 0; req < requests_per_thead; req++)
                {
                    TestGuard g(t, link, 1);
                    check(link.queue->info.priority);
                }
            });
        }
    }

    ClassifierPtr c = t.manager->acquire("leader");
    ResourceLink link = c->get("res1");
    t.blockResource(link);
}
