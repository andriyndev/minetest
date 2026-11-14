// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013, 2017 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "mesh_generator_thread.h"
#include <algorithm>
#include "settings.h"
#include "profiler.h"
#include "client.h"
#include "mapblock.h"
#include "mapblock_mesh.h"
#include "map.h"
#include "util/directiontables.h"
#include "porting.h"

/*
	QueuedMeshUpdate
*/

QueuedMeshUpdate::~QueuedMeshUpdate()
{
	delete data;
}

void QueuedMeshUpdate::retrieveBlocks(Map *map, u16 cell_size)
{
	const size_t total = (cell_size+2)*(cell_size+2)*(cell_size+2);
	if (map_blocks.empty())
		map_blocks.resize(total);
	else
		assert(map_blocks.size() == total); // must not change
	size_t i = 0;
	v3s16 pos;
	// order is not important, but it must be consistent
	// note the extra margin!
	for (pos.X = p.X - 1; pos.X <= p.X + cell_size; pos.X++)
	for (pos.Z = p.Z - 1; pos.Z <= p.Z + cell_size; pos.Z++)
	for (pos.Y = p.Y - 1; pos.Y <= p.Y + cell_size; pos.Y++) {
		if (!map_blocks[i]) {
			MapBlock *block = map->getBlockNoCreateNoEx(pos);
			if (block) {
				block->refGrab();
				map_blocks[i] = block;
			}
		}
		i++;
	}
}

bool QueuedMeshUpdate::checkSkip(u16 cell_size)
{
	bool all_air = true;
	const v3s16 p_max = p + v3s16(cell_size);
	assert(!map_blocks.empty());
	for (auto *block : map_blocks) {
		// ignore extra margin
		if (block && block->getPos() >= p && block->getPos() < p_max) {
			all_air &= block->isAir();
		}
	}
	return all_air;
}

void QueuedMeshUpdate::dropBlocks()
{
	for (auto *block : map_blocks) {
		if (block)
			block->refDrop();
	}
	map_blocks.clear();
}

namespace {
	struct DroppingDeleter {
		void operator() (QueuedMeshUpdate *q) {
			if (q)
				q->dropBlocks();
			delete q;
		}
	};

	// Simple helper to avoid messing up the refcounting
	using UnqueuedMeshUpdate = std::unique_ptr<QueuedMeshUpdate, DroppingDeleter>;
}

/*
	MeshUpdateQueue
*/

MeshUpdateQueue::MeshUpdateQueue(Client *client):
	m_client(client)
{
	assert(m_client);
	m_cache_smooth_lighting = g_settings->getBool("smooth_lighting");
	m_cache_enable_water_reflections = g_settings->getBool("enable_water_reflections");
	m_cache_enable_waving_water = g_settings->getBool("enable_waving_water");
}

MeshUpdateQueue::~MeshUpdateQueue()
{
	clear(true);
}

bool MeshUpdateQueue::addBlock(Map *map, v3s16 p, bool ack_block_to_server,
	bool urgent, bool from_neighbor)
{
	assert(std::this_thread::get_id() == m_main_thread);
	// If block that causes update does not exist, skip.
	if (!map->getBlockNoCreateNoEx(p))
		return false;

	const MeshGrid mesh_grid = m_client->getMeshGrid();

	// Mesh is placed at the corner block of a chunk
	// (where all coordinate are divisible by the chunk size)
	const v3s16 mesh_position = mesh_grid.getMeshPos(p);

	/*
		Find if block is already in queue.
		If it is, update the data and quit.
	*/
	for (QueuedMeshUpdate *q : m_pending) {
		if (q->p == mesh_position) {
			if (ack_block_to_server)
				q->ack_list.push_back(p);
			q->crack_level = m_client->getCrackLevel();
			q->crack_pos = m_client->getCrackPos();
			q->urgent |= urgent;
			q->retrieveBlocks(map, mesh_grid.cell_size);
			return true;
		}
	}

	/*
		Grab the relevant blocks first
	*/
	UnqueuedMeshUpdate q{new QueuedMeshUpdate()};
	q->p = mesh_position;
	if (ack_block_to_server)
		q->ack_list.push_back(p);
	q->crack_level = m_client->getCrackLevel();
	q->crack_pos = m_client->getCrackPos();
	q->urgent = urgent;
	q->retrieveBlocks(map, mesh_grid.cell_size);

	/*
		Air blocks won't suddenly become visible due to a neighbor update, so
		skip those.
		Note: this can be extended with more precise checks in the future
	*/
	if (from_neighbor && q->checkSkip(mesh_grid.cell_size)) {
		assert(!ack_block_to_server);
		g_profiler->add("MeshUpdateQueue: updates skipped", 1);
		return true;
	}

	// Put into queue, pointer moved from `q`.
	m_pending.push_back(q.release());

	return true;
}

void MeshUpdateQueue::reAddBlock(MeshUpdateResult &r)
{
	assert(std::this_thread::get_id() == m_main_thread);
	const MeshGrid mesh_grid = m_client->getMeshGrid();

	const v3s16 mesh_position = mesh_grid.getMeshPos(r.p);

	// Update if exists in queue
	for (QueuedMeshUpdate *q : m_pending) {
		if (q->p == mesh_position) {
			for (auto p : r.ack_list)
				q->ack_list.push_back(p);
			q->urgent |= r.urgent;
			// Do not retrieve blocks again, but make sure to drop our old ones
			r.dropBlocks();
			return;
		}
	}

	// Queue a new update
	UnqueuedMeshUpdate q{new QueuedMeshUpdate()};
	q->p = mesh_position;
	q->ack_list = std::move(r.ack_list);
	q->crack_level = m_client->getCrackLevel();
	q->crack_pos = m_client->getCrackPos();
	q->urgent = r.urgent;
	q->map_blocks = std::move(r.map_blocks);

	m_pending.push_back(q.release());
}

void MeshUpdateQueue::advanceEpoch(u16 epoch)
{
	assert(std::this_thread::get_id() == m_main_thread);
	m_epoch = epoch;
	std::deque<QueuedMeshUpdate *> outdated, outdated_urgent;
	{
		MutexAutoLock lock(m_mutex);
		outdated_urgent.swap(m_queue_urgent);
		outdated.swap(m_queue);
		for (QueuedMeshUpdate *q : outdated_urgent)
			m_inflight_blocks.erase(q->p);
		for (QueuedMeshUpdate *q : outdated)
			m_inflight_blocks.erase(q->p);
	}
	outdated.insert(outdated.begin(), outdated_urgent.begin(), outdated_urgent.end());
	// Ready jobs have not been claimed. Return them to the main-thread
	// queue, merging acknowledgements into any newer pending request.
	for (auto *q : outdated) {
		delete q->data;
		q->data = nullptr;
		auto it = std::find_if(m_pending.begin(), m_pending.end(),
				[q](const auto *pending) { return pending->p == q->p; });
		if (it != m_pending.end()) {
			(*it)->ack_list.insert((*it)->ack_list.end(),
					q->ack_list.begin(), q->ack_list.end());
			(*it)->urgent |= q->urgent;
			q->dropBlocks();
			delete q;
		} else {
			m_pending.push_back(q);
		}
	}
}

bool MeshUpdateQueue::makeRoomForJob(QueuedMeshUpdate *q, size_t max_ready)
{
	assert(std::this_thread::get_id() == m_main_thread);
	UnqueuedMeshUpdate replaced;
	{
		MutexAutoLock lock(m_mutex);
		if (m_inflight_blocks.count(q->p)) {
			for (auto *queue : {&m_queue_urgent, &m_queue}) {
				auto ready = std::find_if(queue->begin(), queue->end(),
						[q](const auto *job) { return job->p == q->p; });
				if (ready == queue->end())
					continue;
				// Promotion needs space in the urgent queue. Same-priority
				// replacement does not increase either queue's size.
				if (q->urgent && !(*ready)->urgent && m_queue_urgent.size() >= max_ready)
					return false;
				replaced.reset(*ready);
				queue->erase(ready);
				break;
			}
			if (!replaced)
				return false; // Already running; keep the newer request pending.
			// Keep the reservation while rebuilding the snapshot.
		} else if ((q->urgent ? m_queue_urgent : m_queue).size() >= max_ready) {
			return false;
		}
	}
	if (replaced) {
		q->ack_list.insert(q->ack_list.end(),
				replaced->ack_list.begin(), replaced->ack_list.end());
		q->urgent |= replaced->urgent;
		// Keep the newer blocks and crack state. Release the old snapshot
		// and block references on the main thread, outside the worker mutex.
		replaced.reset();
	}

	return true;
}

bool MeshUpdateQueue::prepareJob(QueuedMeshUpdate *q, size_t max_ready)
{
	if (!makeRoomForJob(q, max_ready))
		return false;
	fillDataFromMapBlocks(q);
	q->epoch = m_epoch;
	{
		MutexAutoLock lock(m_mutex);
		m_inflight_blocks.insert(q->p);
		(q->urgent ? m_queue_urgent : m_queue).push_back(q);
	}
	return true;
}

void MeshUpdateQueue::prepare(size_t max_ready)
{
	assert(std::this_thread::get_id() == m_main_thread);
	const u64 started = porting::getTimeUs();
	const u64 deadline = started + 2000;
	size_t prepared = 0;
	bool budget_exhausted = false;

	for (bool urgent : {true, false}) {
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			if ((*it)->urgent != urgent || !prepareJob(*it, max_ready)) {
				++it;
				continue;
			}
			// prepareJob published the job; do not dereference it again.
			it = m_pending.erase(it);
			++prepared;
			if (porting::getTimeUs() >= deadline) {
				budget_exhausted = true;
				break;
			}
		}
		if (budget_exhausted)
			break;
	}

	const float elapsed_us = porting::getTimeUs() - started;
	// Sample once per client step, including idle steps. Keep profiler locking
	// outside the queue lock so workers can continue consuming prepared jobs.
	size_t ready, urgent, running;
	{
		MutexAutoLock lock(m_mutex);
		ready = m_queue.size();
		urgent = m_queue_urgent.size();
		running = m_inflight_blocks.size() - ready - urgent;
	}
	g_profiler->add("Client: Mesh input preparation (sum)", elapsed_us / 1000.f);
	g_profiler->max("Mesh preparation: step [us]", elapsed_us);
	g_profiler->add("Mesh preparation: jobs", prepared);
	g_profiler->add("Mesh preparation: budget exhausted", budget_exhausted);
	g_profiler->avg("Mesh queue: pending", m_pending.size());
	g_profiler->max("Mesh queue: pending peak", m_pending.size());
	g_profiler->avg("Mesh queue: ready normal", ready);
	g_profiler->avg("Mesh queue: ready urgent", urgent);
	g_profiler->avg("Mesh queue: running", running);
}

// Returned pointer must be deleted
// Returns NULL if no prepared, eligible job is available
QueuedMeshUpdate *MeshUpdateQueue::pop()
{
	MutexAutoLock lock(m_mutex);
	auto &queue = m_queue_urgent.empty() ? m_queue : m_queue_urgent;
	if (queue.empty())
		return nullptr;

	auto *q = queue.front();
	assert(q->data);
	queue.pop_front();
	return q;
}

void MeshUpdateQueue::done(v3s16 pos)
{
	MutexAutoLock lock(m_mutex);
	m_inflight_blocks.erase(pos);
}

void MeshUpdateQueue::clear(bool finish)
{
	assert(std::this_thread::get_id() == m_main_thread);
	const auto clear_queue = [finish](auto &queue, auto on_remove) {
		// Points to the current position for inserting retained jobs.
		// Retained jobs are moved to the beginning of the container, and then it's trimmed.
		auto out = queue.begin();
		for (QueuedMeshUpdate *it : queue) {
			// If we're in an active game session clearing updates that the
			// server expects us to ack will cause problems.
			if (it->ack_list.empty() || finish) {
				on_remove(it);
				it->dropBlocks();
				delete it;
			} else {
				*out++ = it;
			}
		}
		queue.erase(out, queue.end());
	};
	clear_queue(m_pending, [](QueuedMeshUpdate *) {});
	MutexAutoLock lock(m_mutex);
	clear_queue(m_queue_urgent, [this](QueuedMeshUpdate *q) { m_inflight_blocks.erase(q->p); });
	clear_queue(m_queue, [this](QueuedMeshUpdate *q) { m_inflight_blocks.erase(q->p); });
}

void MeshUpdateQueue::fillDataFromMapBlocks(QueuedMeshUpdate *q)
{
	assert(std::this_thread::get_id() == m_main_thread);
	auto mesh_grid = m_client->getMeshGrid();
	MeshMakeData *data = new MeshMakeData(m_client->ndef(),
			MAP_BLOCKSIZE * mesh_grid.cell_size, mesh_grid);
	q->data = data;

	data->fillBlockDataBegin(q->p);

	for (auto *block : q->map_blocks) {
		if (block)
			block->copyToClipped(data->m_vmanip);
	}

	data->setCrack(q->crack_level, q->crack_pos);

	float ao_gamma = m_client->getCommittedStaticLighting().ao_gamma;
	data->m_ao_gamma_inv = ao_gamma == 0.0f ? 0.0f : (1.0f / ao_gamma);

	data->m_generate_minimap = !!m_client->getMinimap();
	data->m_smooth_lighting = m_cache_smooth_lighting;
	data->m_enable_water_reflections = m_cache_enable_water_reflections;
	data->m_enable_waving_water = m_cache_enable_waving_water;
}

/*
	MeshUpdateResult
*/

void MeshUpdateResult::dropBlocks()
{
	for (auto *block : map_blocks) {
		if (block)
			block->refDrop();
	}
	map_blocks.clear();
}

/*
	MeshUpdateWorkerThread
*/

MeshUpdateWorkerThread::MeshUpdateWorkerThread(Client *client, MeshUpdateQueue *queue_in, MeshUpdateManager *manager) :
		UpdateThread("Mesh"), m_client(client), m_queue_in(queue_in), m_manager(manager)
{
	m_generation_interval = g_settings->getU16("mesh_generation_interval");
	m_generation_interval = rangelim(m_generation_interval, 0, 25);
}

void MeshUpdateWorkerThread::doUpdate()
{
	assert(Thread::getCurrentThread() == this);
	QueuedMeshUpdate *q;
	while ((q = m_queue_in->pop())) {
		ScopeProfiler sp(g_profiler, "Client: Mesh making (sum)");

		// This generates the mesh:
		MapBlockMesh *mesh_new = new MapBlockMesh(m_client, q->data);

		MeshUpdateResult r;
		r.p = q->p;
		r.mesh = std::unique_ptr<MapBlockMesh>(mesh_new);
		r.solid_sides = get_solid_sides(q->data);
		r.ack_list = std::move(q->ack_list);
		r.map_blocks = std::move(q->map_blocks);
		r.epoch = q->epoch;
		r.urgent = q->urgent;

		m_manager->putResult(std::move(r));
		m_queue_in->done(q->p);
		delete q;
		sp.stop();

		porting::TriggerMemoryTrim();

		// do this after we're done so the interval is enforced without
		// adding extra latency.
		if (m_generation_interval)
			sleep_ms(m_generation_interval);
	}
}

/*
	MeshUpdateManager
*/

MeshUpdateManager::MeshUpdateManager(Client *client):
	m_queue_in(client)
{
	int number_of_threads = rangelim(g_settings->getS32("mesh_generation_threads"), 0, 8);

	// Automatically use 25% of the system cores for mesh generation, max 3
	if (number_of_threads == 0)
		number_of_threads = std::min(3U, Thread::getNumberOfProcessors() / 4);

	// use at least one thread
	number_of_threads = std::max(1, number_of_threads);
	infostream << "MeshUpdateManager: using " << number_of_threads << " threads" << std::endl;

	for (int i = 0; i < number_of_threads; i++)
		m_workers.push_back(std::make_unique<MeshUpdateWorkerThread>(client, &m_queue_in, this));
}

void MeshUpdateManager::updateBlock(Map *map, v3s16 p, bool ack_block_to_server,
		bool urgent, bool update_neighbors)
{
	assert(std::this_thread::get_id() == m_main_thread);
	static thread_local const bool many_neighbors =
			g_settings->getBool("smooth_lighting")
			&& !g_settings->getFlag("performance_tradeoffs");
	if (!m_queue_in.addBlock(map, p, ack_block_to_server, urgent, false)) {
		warningstream << "Update requested for non-existent block at "
				<< p << std::endl;
		return;
	}
	if (update_neighbors) {
		if (many_neighbors) {
			for (v3s16 dp : g_26dirs)
				m_queue_in.addBlock(map, p + dp, false, urgent, true);
		} else {
			for (v3s16 dp : g_6dirs)
				m_queue_in.addBlock(map, p + dp, false, urgent, true);
		}
	}
}

void MeshUpdateManager::prepare()
{
	assert(std::this_thread::get_id() == m_main_thread);
	// Keep enough inputs for workers to continue between client steps without
	// allocating snapshots for the entire pending queue.
	m_queue_in.prepare(2 * m_workers.size());
	deferUpdate();
}

void MeshUpdateManager::putResult(MeshUpdateResult &&result)
{
	if (result.urgent)
		m_queue_out_urgent.push_back(std::move(result));
	else
		m_queue_out.push_back(std::move(result));
}

bool MeshUpdateManager::getNextResult(MeshUpdateResult &r)
{
	assert(std::this_thread::get_id() == m_main_thread);
	if (!m_queue_out_urgent.empty())
		r = m_queue_out_urgent.pop_frontNoEx();
	else if (!m_queue_out.empty())
		r = m_queue_out.pop_frontNoEx();
	else
		return false;

	if (r.epoch != m_epoch) {
		// just kidding, we can't return this result. put it back into the queue.
		verbosestream << "MeshUpdateManager: have to regenerate " << r.p
			<< " due to epoch difference (" << r.epoch << ")" << std::endl;
		m_queue_in.reAddBlock(r);
		r = MeshUpdateResult(); // don't leave stale data to caller
		return getNextResult(r);
	}
	return true;
}

void MeshUpdateManager::forceRegenerate(Map *map, const std::vector<v3s16> &pp)
{
	assert(std::this_thread::get_id() == m_main_thread);
	// This invalidates all previously produced results (if still pending)
	m_queue_in.advanceEpoch(++m_epoch);

	verbosestream << "MeshUpdateManager: m_epoch=" << m_epoch << "; will queue "
		<< pp.size() << " blocks to be updated" << std::endl;

	for (v3s16 p : pp) {
		m_queue_in.addBlock(map, p, false, false, false);
	}
}

void MeshUpdateManager::clearAllQueues(bool finish)
{
	assert(std::this_thread::get_id() == m_main_thread);
	m_queue_in.clear(finish);

	// Same problem as in MeshUpdateQueue::clear() here: we can't just blindly
	// throw away results that the server expects to receive an ack for.
	const auto &do_it = [finish] (ResultQueue &queue) {
		auto helper = queue.iterLocked();
		for (auto it = helper.begin(); it != helper.end(); ) {
			if (it->ack_list.empty() || finish) {
				it->dropBlocks();
				it = helper.erase(it);
			} else {
				++it;
			}
		}
	};
	do_it(m_queue_out_urgent);
	do_it(m_queue_out);
}

void MeshUpdateManager::deferUpdate()
{
	assert(std::this_thread::get_id() == m_main_thread);
	for (auto &thread : m_workers)
		thread->deferUpdate();
}

void MeshUpdateManager::start()
{
	assert(std::this_thread::get_id() == m_main_thread);
	for (auto &thread: m_workers)
		thread->start();
}

void MeshUpdateManager::stop()
{
	assert(std::this_thread::get_id() == m_main_thread);
	for (auto &thread: m_workers)
		thread->stop();
}

void MeshUpdateManager::wait()
{
	assert(std::this_thread::get_id() == m_main_thread);
	for (auto &thread: m_workers)
		thread->wait();
}

bool MeshUpdateManager::isRunning()
{
	for (auto &thread: m_workers)
		if (thread->isRunning())
			return true;
	return false;
}
