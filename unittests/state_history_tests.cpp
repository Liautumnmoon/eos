#include <boost/test/unit_test.hpp>
#include <contracts.hpp>
#include <eosio/state_history/log.hpp>
#include <eosio/state_history/trace_converter.hpp>
#include <eosio/testing/tester.hpp>
#include <fc/io/json.hpp>

#include "test_cfd_transaction.hpp"
#include <boost/filesystem.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>

using namespace eosio;
using namespace testing;
using namespace chain;
namespace bio = boost::iostreams;


state_history::partial_transaction_v0 get_partial_from_traces(std::vector<state_history::transaction_trace>& traces, const transaction_id_type& id) {
   auto cfd_trace_itr = std::find_if(traces.begin(), traces.end(), [id](const state_history::transaction_trace& v) {
      return v.get<state_history::transaction_trace_v0>().id == id;
   });

   // make sure the trace with cfd can be found
   BOOST_REQUIRE(cfd_trace_itr != traces.end());
   BOOST_REQUIRE(cfd_trace_itr->contains<state_history::transaction_trace_v0>());
   auto trace_v0 = cfd_trace_itr->get<state_history::transaction_trace_v0>();
   BOOST_REQUIRE(trace_v0.partial);
   BOOST_REQUIRE(trace_v0.partial->contains<state_history::partial_transaction_v0>());
   return trace_v0.partial->get<state_history::partial_transaction_v0>();   
}

state_history::partial_transaction_v0 get_partial_from_traces_bin(const std::vector<char>& entry,
                                                                  const transaction_id_type& id) {
   fc::datastream<const char*> strm(entry.data(), entry.size());
   std::vector<state_history::transaction_trace> traces;
   state_history::trace_converter::unpack(strm, traces);
   return get_partial_from_traces(traces, id);
}

state_history::partial_transaction_v0 get_partial_from_serialized_traces(const std::vector<char>& entry,
                                                                         const transaction_id_type& id) {
   fc::datastream<const char*> strm(entry.data(), entry.size());
   std::vector<state_history::transaction_trace> traces;
   fc::raw::unpack(strm, traces);
   return get_partial_from_traces(traces, id);
}

BOOST_AUTO_TEST_SUITE(test_state_history)

BOOST_AUTO_TEST_CASE(test_trace_converter) {

   tester chain;
   using namespace eosio::state_history;

   state_history::transaction_trace_cache cache;
   std::map<uint32_t, bytes>      on_disk_log_entries;

   chain.control->applied_transaction.connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          cache.add_transaction(std::get<0>(t), std::get<1>(t));
       });

   chain.control->accepted_block.connect([&](const block_state_ptr& bs) {
         auto traces = cache.prepare_traces(bs);
         fc::datastream<std::vector<char>> strm;
         state_history::trace_converter::pack(strm, chain.control->db(), true, traces, state_history::compression_type::zlib);
         on_disk_log_entries[bs->block_num] = strm.storage();
   });

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);
   chain.produce_blocks(1);

   BOOST_CHECK(on_disk_log_entries.size());

   // Now deserialize the on disk trace log and make sure that the cfd exists
   auto& cfd_entry = on_disk_log_entries.at(cfd_trace->block_num);
   auto  partial =
       get_partial_from_traces_bin(cfd_entry, cfd_trace->id);
   BOOST_REQUIRE(partial.context_free_data.size());
   BOOST_REQUIRE(partial.signatures.size());

   // prune the cfd for the block
   std::vector<transaction_id_type> ids{cfd_trace->id};
   fc::datastream<char*> rw_strm(cfd_entry.data(), cfd_entry.size());
   state_history::trace_converter::prune_traces(rw_strm, cfd_entry.size(), ids);
   BOOST_CHECK(ids.size() == 0);

   // read the pruned trace and make sure the signature/cfd are empty
   auto pruned_partial =
       get_partial_from_traces_bin(cfd_entry, cfd_trace->id);
   BOOST_CHECK(pruned_partial.context_free_data.size() == 0);
   BOOST_CHECK(pruned_partial.signatures.size() == 0);
}

BOOST_AUTO_TEST_CASE(test_trace_log) {
   namespace bfs = boost::filesystem;
   tester chain;

   scoped_temp_path state_history_dir;
   fc::create_directories(state_history_dir.path);
   state_history_traces_log log(state_history_dir.path);

   chain.control->applied_transaction.connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          log.add_transaction(std::get<0>(t), std::get<1>(t));
       });

   chain.control->accepted_block.connect([&](const block_state_ptr& bs) { log.store(chain.control->db(), bs); });

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);
   chain.produce_blocks(1);

   auto traces = log.get_traces(cfd_trace->block_num);
   BOOST_REQUIRE(traces.size());

   auto partial = get_partial_from_traces(traces, cfd_trace->id);
   BOOST_REQUIRE(partial.context_free_data.size());
   BOOST_REQUIRE(partial.signatures.size());

   std::vector<transaction_id_type> ids{cfd_trace->id};
   log.prune_transactions(cfd_trace->block_num, ids);
   BOOST_REQUIRE(ids.empty());

   // we assume the nodeos has to be stopped while running, it can only be read
   // correctly with restart
   state_history_traces_log new_log(state_history_dir.path);
   auto                     pruned_traces = new_log.get_traces(cfd_trace->block_num);
   BOOST_REQUIRE(pruned_traces.size());

   auto pruned_partial = get_partial_from_traces(pruned_traces, cfd_trace->id);
   BOOST_CHECK(pruned_partial.context_free_data.empty());
   BOOST_CHECK(pruned_partial.signatures.empty());
}

BOOST_AUTO_TEST_SUITE_END()
