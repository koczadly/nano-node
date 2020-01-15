#include <nano/node/node.hpp>
#include <nano/node/repcrawler.hpp>

#include <boost/format.hpp>

nano::rep_crawler::rep_crawler (nano::node & node_a) :
node (node_a)
{
	if (!node.flags.disable_rep_crawler)
	{
		node.observers.endpoint.add ([this](std::shared_ptr<nano::transport::channel> channel_a) {
			this->query (channel_a);
		});
	}
}

void nano::rep_crawler::add (nano::block_hash const & hash_a)
{
	nano::lock_guard<std::mutex> lock (active_mutex);
	active.insert (hash_a);
}

void nano::rep_crawler::remove (nano::block_hash const & hash_a)
{
	nano::lock_guard<std::mutex> lock (active_mutex);
	active.erase (hash_a);
}

void nano::rep_crawler::start ()
{
	ongoing_crawl ();
}

void nano::rep_crawler::validate ()
{
	decltype (responses) responses_l;
	{
		nano::lock_guard<std::mutex> lock (active_mutex);
		responses_l.swap (responses);
	}
	auto minimum = node.minimum_principal_weight ();
	for (auto const & i: responses_l)
	{
		auto & vote = i.second;
		auto & channel = i.first;
		nano::uint128_t rep_weight = node.ledger.weight (vote->account);
		if (rep_weight > minimum)
		{
			auto updated_or_inserted = false;
			nano::unique_lock<std::mutex> lock (probable_reps_mutex);
			auto existing (probable_reps.find (vote->account));
			if (existing != probable_reps.end ())
			{
				probable_reps.modify (existing, [rep_weight, &updated_or_inserted, &vote, &channel](nano::representative & info) {
					info.last_response = std::chrono::steady_clock::now ();

					// Update if representative channel was changed
					if (info.channel->get_endpoint () != channel->get_endpoint ())
					{
						assert (info.account == vote->account);
						updated_or_inserted = true;
						info.weight = rep_weight;
						info.channel = channel;
					}
				});
			}
			else
			{
				probable_reps.emplace (nano::representative (vote->account, rep_weight, channel));
				updated_or_inserted = true;
			}
			lock.unlock ();
			if (updated_or_inserted)
			{
				node.logger.try_log (boost::str (boost::format ("Found a representative at %1%") % channel->to_string ()));
				// Rebroadcasting all active votes to new representative
				auto blocks (node.active.list_blocks ());
				for (auto i (blocks.begin ()), n (blocks.end ()); i != n; ++i)
				{
					if (*i != nullptr)
					{
						nano::confirm_req req (*i);
						channel->send (req);
					}
				}
			}
		}
	}
}

void nano::rep_crawler::ongoing_crawl ()
{
	auto now (std::chrono::steady_clock::now ());
	auto total_weight_l (total_weight ());
	cleanup_reps ();
	update_weights ();
	validate ();
	query (get_crawl_targets (total_weight_l));
	auto sufficient_weight (total_weight_l > node.config.online_weight_minimum.number ());
	// If online weight drops below minimum, reach out to preconfigured peers
	if (!sufficient_weight)
	{
		node.keepalive_preconfigured (node.config.preconfigured_peers);
	}
	// Reduce crawl frequency when there's enough total peer weight
	unsigned next_run_ms = node.network_params.network.is_test_network () ? 100 : sufficient_weight ? 7000 : 3000;
	std::weak_ptr<nano::node> node_w (node.shared ());
	node.alarm.add (now + std::chrono::milliseconds (next_run_ms), [node_w, this]() {
		if (auto node_l = node_w.lock ())
		{
			this->ongoing_crawl ();
		}
	});
}

std::vector<std::shared_ptr<nano::transport::channel>> nano::rep_crawler::get_crawl_targets (nano::uint128_t total_weight_a)
{
	constexpr size_t conservative_count = 10;
	constexpr size_t aggressive_count = 40;

	// Crawl more aggressively if we lack sufficient total peer weight.
	bool sufficient_weight (total_weight_a > node.config.online_weight_minimum.number ());
	uint16_t required_peer_count = sufficient_weight ? conservative_count : aggressive_count;

	// Add random peers. We do this even if we have enough weight, in order to pick up reps
	// that didn't respond when first observed. If the current total weight isn't sufficient, this
	// will be more aggressive. When the node first starts, the rep container is empty and all
	// endpoints will originate from random peers.
	required_peer_count += required_peer_count / 2;

	// The rest of the endpoints are picked randomly
	auto random_peers (node.network.random_set (required_peer_count));
	std::vector<std::shared_ptr<nano::transport::channel>> result;
	result.insert (result.end (), random_peers.begin (), random_peers.end ());
	return result;
}

void nano::rep_crawler::query (std::vector<std::shared_ptr<nano::transport::channel>> const & channels_a)
{
	auto transaction (node.store.tx_begin_read ());
	std::shared_ptr<nano::block> block (node.store.block_random (transaction));
	auto hash (block->hash ());
	{
		nano::lock_guard<std::mutex> lock (active_mutex);
		// Don't send same block multiple times in tests
		if (node.network_params.network.is_test_network ())
		{
			for (auto i (0); active.count (hash) != 0 && i < 4; ++i)
			{
				block = node.store.block_random (transaction);
				hash = block->hash ();
			}
		}
		active.insert (hash);
	}
	for (auto i (channels_a.begin ()), n (channels_a.end ()); i != n; ++i)
	{
		on_rep_request (*i);
		node.network.send_confirm_req (*i, block);
	}

	// A representative must respond with a vote within the deadline
	std::weak_ptr<nano::node> node_w (node.shared ());
	node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w, hash]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->rep_crawler.remove (hash);
		}
	});
}

void nano::rep_crawler::query (std::shared_ptr<nano::transport::channel> channel_a)
{
	std::vector<std::shared_ptr<nano::transport::channel>> peers;
	peers.push_back (channel_a);
	query (peers);
}

void nano::rep_crawler::response (std::shared_ptr<nano::transport::channel> channel_a, std::shared_ptr<nano::vote> vote_a)
{
	nano::lock_guard<std::mutex> lock (active_mutex);
	for (auto i = vote_a->begin (), n = vote_a->end (); i != n; ++i)
	{
		if (active.count (*i) != 0)
		{
			responses.push_back (std::make_pair (channel_a, vote_a));
			break;
		}
	}
}

nano::uint128_t nano::rep_crawler::total_weight () const
{
	nano::lock_guard<std::mutex> lock (probable_reps_mutex);
	nano::uint128_t result (0);
	for (auto i (probable_reps.get<tag_weight> ().begin ()), n (probable_reps.get<tag_weight> ().end ()); i != n; ++i)
	{
		auto weight (i->weight.number ());
		if (weight > 0)
		{
			result = result + weight;
		}
		else
		{
			break;
		}
	}
	return result;
}

void nano::rep_crawler::on_rep_request (std::shared_ptr<nano::transport::channel> channel_a)
{
	nano::lock_guard<std::mutex> lock (probable_reps_mutex);

	probably_rep_t::index<tag_channel_ref>::type & channel_ref_index = probable_reps.get<tag_channel_ref> ();

	// Find and update the timestamp on all reps available on the endpoint (a single host may have multiple reps)
	auto itr_pair = probable_reps.get<tag_channel_ref> ().equal_range (*channel_a);
	for (; itr_pair.first != itr_pair.second; itr_pair.first++)
	{
		channel_ref_index.modify (itr_pair.first, [](nano::representative & value_a) {
			value_a.last_request = std::chrono::steady_clock::now ();
		});
	}
}

void nano::rep_crawler::cleanup_reps ()
{
	std::vector<std::shared_ptr<nano::transport::channel>> channels;
	{
		// Check known rep channels
		nano::lock_guard<std::mutex> lock (probable_reps_mutex);
		for (auto i (probable_reps.get<tag_last_request> ().begin ()), n (probable_reps.get<tag_last_request> ().end ()); i != n; ++i)
		{
			channels.push_back (i->channel);
		}
	}
	// Remove reps with inactive channels
	for (auto i : channels)
	{
		bool equal (false);
		if (i->get_type () == nano::transport::transport_type::tcp)
		{
			auto find_channel (node.network.tcp_channels.find_channel (i->get_tcp_endpoint ()));
			if (find_channel != nullptr && *find_channel == *static_cast<nano::transport::channel_tcp *> (i.get ()))
			{
				equal = true;
			}
		}
		else if (i->get_type () == nano::transport::transport_type::udp)
		{
			auto find_channel (node.network.udp_channels.channel (i->get_endpoint ()));
			if (find_channel != nullptr && *find_channel == *static_cast<nano::transport::channel_udp *> (i.get ()))
			{
				equal = true;
			}
		}
		if (!equal)
		{
			nano::lock_guard<std::mutex> lock (probable_reps_mutex);
			probable_reps.get<tag_channel_ref> ().erase (*i);
		}
	}
}

void nano::rep_crawler::update_weights ()
{
	nano::lock_guard<std::mutex> lock (probable_reps_mutex);
	for (auto i (probable_reps.get<tag_last_request> ().begin ()), n (probable_reps.get<tag_last_request> ().end ()); i != n;)
	{
		auto weight (node.ledger.weight (i->account));
		if (weight > 0)
		{
			if (i->weight.number () != weight)
			{
				probable_reps.get<tag_last_request> ().modify (i, [weight](nano::representative & info) {
					info.weight = weight;
				});
			}
			++i;
		}
		else
		{
			// Erase non representatives
			i = probable_reps.get<tag_last_request> ().erase (i);
		}
	}
}

std::vector<nano::representative> nano::rep_crawler::representatives (size_t count_a, boost::optional<decltype (nano::protocol_constants::protocol_version_min)> const & opt_version_min_a)
{
	auto version_min (opt_version_min_a.value_or (node.network_params.protocol.protocol_version_min));
	std::vector<representative> result;
	nano::lock_guard<std::mutex> lock (probable_reps_mutex);
	for (auto i (probable_reps.get<tag_weight> ().begin ()), n (probable_reps.get<tag_weight> ().end ()); i != n && result.size () < count_a; ++i)
	{
		if (!i->weight.is_zero () && i->channel->get_network_version () >= version_min)
		{
			result.push_back (*i);
		}
	}
	return result;
}

std::vector<std::shared_ptr<nano::transport::channel>> nano::rep_crawler::representative_endpoints (size_t count_a)
{
	std::vector<std::shared_ptr<nano::transport::channel>> result;
	auto reps (representatives (count_a));
	for (auto rep : reps)
	{
		result.push_back (rep.channel);
	}
	return result;
}

/** Total number of representatives */
size_t nano::rep_crawler::representative_count ()
{
	nano::lock_guard<std::mutex> lock (probable_reps_mutex);
	return probable_reps.size ();
}
