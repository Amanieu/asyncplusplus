#include <async++.h>
#include <cassert>
#include <tuple>
#include <vector>

int main()
{
    std::vector<async::task<int>> tasks;
    tasks.push_back(async::make_task(1));
    tasks.push_back(async::make_task(2));
    auto result = async::when_any(tasks.begin(), tasks.end()).get();
    assert(result.index == 0);
    assert(result.tasks.size() == 2);
    assert(result.tasks[0].get() == 1);
    assert(result.tasks[1].get() == 2);

    auto first = async::make_task(3);
    async::event_task<int> event;
    auto second = event.get_task();
    auto tuple = async::when_any(first, second).get();
    assert(tuple.index == 0);
    assert(std::get<0>(tuple.tasks).get() == 3);
    assert(std::get<1>(tuple.tasks).valid());
    event.set(4);
    assert(std::get<1>(tuple.tasks).get() == 4);

    async::event_task<int> pending1, pending2;
    std::vector<async::task<int>> pending;
    pending.push_back(pending1.get_task());
    pending.push_back(pending2.get_task());
    auto any = async::when_any(pending.begin(), pending.end());
    assert(!any.ready());
    pending2.set(6);
    auto completed = any.get();
    assert(completed.index == 1);
    assert(completed.tasks.size() == 2);
    pending1.set(5);
    assert(completed.tasks[0].get() == 5);
    assert(completed.tasks[1].get() == 6);

    std::vector<async::task<int>> empty;
    assert(async::when_any(empty.begin(), empty.end()).get().tasks.empty());
}
