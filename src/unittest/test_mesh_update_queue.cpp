// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test.h"
#include "client/mesh_generator_thread.h"
#include "client/mapblock_mesh.h"
#include "mapblock.h"
#include <future>
#include <thread>

class TestMeshUpdateQueue : public TestBase {
public:
	TestMeshUpdateQueue() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestMeshUpdateQueue"; }
	void runTests(IGameDef *gamedef) override;

private:
	struct Deleter {
		void operator()(QueuedMeshUpdate *q) const
		{
			q->dropBlocks();
			delete q;
		}
	};
	using Job = std::unique_ptr<QueuedMeshUpdate, Deleter>;

	static Job job(s16 x, bool urgent = false)
	{
		Job q(new QueuedMeshUpdate);
		q->p = {x, 0, 0};
		q->urgent = urgent;
		return q;
	}

	static void publish(MeshUpdateQueue &queue, Job q)
	{
		q->data = new MeshMakeData(nullptr, MAP_BLOCKSIZE, MeshGrid{1});
		queue.m_inflight_blocks.insert(q->p);
		(q->urgent ? queue.m_queue_urgent : queue.m_queue).push_back(q.release());
	}

	static Job pop(MeshUpdateQueue &queue)
	{
		return Job(queue.pop());
	}

	void testPriorityAndRunning();
	void testCapacity();
	void testWorkerHandoff();
	void testReplacement(IGameDef *gamedef);
	void testEpoch(IGameDef *gamedef);
	void testClear(IGameDef *gamedef);
};

static TestMeshUpdateQueue g_test_instance;

void TestMeshUpdateQueue::runTests(IGameDef *gamedef)
{
	TEST(testPriorityAndRunning);
	TEST(testCapacity);
	TEST(testWorkerHandoff);
	TEST(testReplacement, gamedef);
	TEST(testEpoch, gamedef);
	TEST(testClear, gamedef);
}

void TestMeshUpdateQueue::testPriorityAndRunning()
{
	MeshUpdateQueue queue;
	publish(queue, job(1));
	publish(queue, job(2, true));
	publish(queue, job(3, true));
	publish(queue, job(4));
	for (s16 x : {2, 3, 1, 4}) {
		auto running = pop(queue);
		UASSERT(running);
		UASSERTEQ(v3s16, running->p, v3s16(x, 0, 0));
		auto newer = job(x);
		UASSERT(!queue.makeRoomForJob(newer.get(), 4));
		queue.done(running->p);
		UASSERT(queue.makeRoomForJob(newer.get(), 4));
	}
	UASSERT(!pop(queue));
	UASSERT(queue.m_inflight_blocks.empty());
}

void TestMeshUpdateQueue::testCapacity()
{
	MeshUpdateQueue queue;
	publish(queue, job(1));
	auto normal = job(2);
	auto urgent = job(3, true);
	UASSERT(!queue.makeRoomForJob(normal.get(), 1));
	UASSERT(queue.makeRoomForJob(urgent.get(), 1));
	publish(queue, std::move(urgent));
	auto another_urgent = job(4, true);
	UASSERT(!queue.makeRoomForJob(another_urgent.get(), 1));

	// Same-priority replacements remain possible with both queues full.
	for (s16 x : {1, 3}) {
		auto replacement = job(x, x == 3);
		UASSERT(queue.makeRoomForJob(replacement.get(), 1));
		publish(queue, std::move(replacement));
	}
	UASSERTEQ(size_t, queue.size(), 2);
	auto first = pop(queue);
	UASSERTEQ(v3s16, first->p, v3s16(3, 0, 0));
	queue.done(first->p);
	UASSERT(!queue.makeRoomForJob(normal.get(), 1));
	UASSERT(queue.makeRoomForJob(another_urgent.get(), 1));
}

void TestMeshUpdateQueue::testWorkerHandoff()
{
	MeshUpdateQueue queue;
	publish(queue, job(1));
	std::promise<QueuedMeshUpdate *> claimed;
	auto claimed_job = claimed.get_future();
	std::promise<void> release;
	auto released = release.get_future();
	std::thread worker([&] {
		auto *q = queue.pop();
		claimed.set_value(q);
		released.wait();
		if (q)
			queue.done(q->p);
	});

	// Keep the worker alive with a claimed job while the main thread changes
	// queue state. Handshakes avoid timing assumptions or sleeps.
	Job running(claimed_job.get());
	auto newer = job(1, true);
	const bool deferred = !queue.makeRoomForJob(newer.get(), 1);
	queue.advanceEpoch(7);
	queue.clear(true);
	const bool still_reserved = queue.m_inflight_blocks.count(v3s16(1, 0, 0));
	release.set_value();
	worker.join();

	// Assert after joining so a failed assertion cannot leave a joinable thread.
	UASSERT(running);
	UASSERT(deferred);
	UASSERT(still_reserved);
	UASSERT(queue.m_inflight_blocks.empty());
	UASSERT(queue.makeRoomForJob(newer.get(), 1));
	publish(queue, std::move(newer));
	auto next = pop(queue);
	UASSERTEQ(v3s16, next->p, v3s16(1, 0, 0));
	UASSERT(next->urgent);
	queue.done(next->p);
}

void TestMeshUpdateQueue::testReplacement(IGameDef *gamedef)
{
	MapBlock old_block({1, 0, 0}, gamedef), new_block({1, 0, 0}, gamedef);
	MeshUpdateQueue queue;
	auto old = job(1);
	old->ack_list = {{1, 0, 0}};
	old->map_blocks.push_back(&old_block);
	old_block.refGrab();
	publish(queue, std::move(old));
	publish(queue, job(2, true));

	auto newer = job(1, true);
	newer->ack_list = {{2, 0, 0}};
	newer->crack_level = 3;
	newer->crack_pos = {4, 5, 6};
	newer->map_blocks.push_back(&new_block);
	new_block.refGrab();
	// Promotion cannot exceed the urgent capacity; both old jobs survive.
	UASSERT(!queue.makeRoomForJob(newer.get(), 1));
	UASSERTEQ(size_t, queue.size(), 2);
	UASSERTEQ(short, old_block.refGet(), 1);
	auto urgent = pop(queue);
	queue.done(urgent->p);
	UASSERT(queue.makeRoomForJob(newer.get(), 1));
	UASSERTEQ(short, old_block.refGet(), 0);
	UASSERTEQ(short, new_block.refGet(), 1);
	UASSERTEQ(size_t, newer->ack_list.size(), 2);
	UASSERTEQ(v3s16, newer->ack_list[0], v3s16(2, 0, 0));
	UASSERTEQ(v3s16, newer->ack_list[1], v3s16(1, 0, 0));
	UASSERTEQ(int, newer->crack_level, 3);
	UASSERTEQ(v3s16, newer->crack_pos, v3s16(4, 5, 6));
	UASSERT(queue.m_inflight_blocks.count(newer->p));
	publish(queue, std::move(newer));

	// An urgent ready job keeps its urgency even if the newer request is normal.
	auto latest = job(1);
	UASSERT(queue.makeRoomForJob(latest.get(), 1));
	UASSERT(latest->urgent);
	UASSERTEQ(size_t, latest->ack_list.size(), 2);
	UASSERTEQ(short, new_block.refGet(), 0);
	publish(queue, std::move(latest));
	auto extra = job(3, true);
	UASSERT(!queue.makeRoomForJob(extra.get(), 1));
}

void TestMeshUpdateQueue::testEpoch(IGameDef *gamedef)
{
	MapBlock block({}, gamedef);
	MeshUpdateQueue queue;
	publish(queue, job(9));
	auto running = pop(queue);
	auto old = job(1, true);
	old->ack_list = {{1, 0, 0}};
	old->map_blocks.push_back(&block);
	block.refGrab();
	publish(queue, std::move(old));
	publish(queue, job(2));
	auto newer = job(1);
	newer->ack_list = {{3, 0, 0}};
	newer->crack_level = 4;
	auto *pending = newer.release();
	queue.m_pending.push_back(pending);
	queue.advanceEpoch(7);
	UASSERTEQ(u16, queue.m_epoch, 7);
	UASSERT(queue.m_queue.empty() && queue.m_queue_urgent.empty());
	UASSERTEQ(size_t, queue.m_pending.size(), 2);
	UASSERT(pending->urgent && !pending->data);
	UASSERTEQ(size_t, pending->ack_list.size(), 2);
	UASSERTEQ(v3s16, pending->ack_list[0], v3s16(3, 0, 0));
	UASSERTEQ(v3s16, pending->ack_list[1], v3s16(1, 0, 0));
	UASSERTEQ(int, pending->crack_level, 4);
	UASSERTEQ(short, block.refGet(), 0);
	UASSERTEQ(size_t, queue.m_inflight_blocks.size(), 1);
	UASSERT(queue.m_inflight_blocks.count(running->p));
	queue.done(running->p);
	queue.advanceEpoch(8);
	UASSERTEQ(size_t, pending->ack_list.size(), 2);
}

void TestMeshUpdateQueue::testClear(IGameDef *gamedef)
{
	MapBlock block({}, gamedef);
	MeshUpdateQueue queue;
	publish(queue, job(9));
	auto running = pop(queue);
	for (s16 x = 0; x < 6; ++x) {
		auto q = job(x, x >= 4);
		q->map_blocks.push_back(&block);
		block.refGrab();
		if (x % 2)
			q->ack_list.push_back(q->p);
		if (x < 2)
			queue.m_pending.push_back(q.release());
		else
			publish(queue, std::move(q));
	}
	queue.clear(false);
	UASSERTEQ(size_t, queue.size(), 3);
	UASSERTEQ(short, block.refGet(), 3);
	UASSERTEQ(v3s16, queue.m_pending.front()->p, v3s16(1, 0, 0));
	UASSERTEQ(v3s16, queue.m_queue.front()->p, v3s16(3, 0, 0));
	UASSERTEQ(v3s16, queue.m_queue_urgent.front()->p, v3s16(5, 0, 0));
	UASSERTEQ(size_t, queue.m_inflight_blocks.size(), 3);
	queue.clear(true);
	UASSERTEQ(size_t, queue.size(), 0);
	UASSERTEQ(short, block.refGet(), 0);
	UASSERTEQ(size_t, queue.m_inflight_blocks.size(), 1);
	queue.done(running->p);
	UASSERT(queue.m_inflight_blocks.empty());
}
