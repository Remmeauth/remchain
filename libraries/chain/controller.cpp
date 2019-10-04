#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <eosio/chain/block_log.hpp>
#include <eosio/chain/fork_database.hpp>
#include <eosio/chain/exceptions.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/block_summary_object.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/protocol_state_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/reversible_block_object.hpp>
#include <eosio/chain/genesis_intrinsics.hpp>
#include <eosio/chain/whitelisted_intrinsics.hpp>

#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/chain_snapshot.hpp>
#include <eosio/chain/thread_utils.hpp>

#include <chainbase/chainbase.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>

namespace eosio { namespace chain {

using resource_limits::resource_limits_manager;

using controller_index_set = index_set<
   account_index,
   account_metadata_index,
   account_ram_correction_index,
   global_property_multi_index,
   protocol_state_multi_index,
   dynamic_global_property_multi_index,
   block_summary_multi_index,
   transaction_multi_index,
   generated_transaction_multi_index,
   table_id_multi_index,
   code_index
>;

using contract_database_index_set = index_set<
   key_value_index,
   index64_index,
   index128_index,
   index256_index,
   index_double_index,
   index_long_double_index
>;

class maybe_session {
   public:
      maybe_session() = default;

      maybe_session( maybe_session&& other)
      :_session(move(other._session))
      {
      }

      explicit maybe_session(database& db) {
         _session = db.start_undo_session(true);
      }

      maybe_session(const maybe_session&) = delete;

      void squash() {
         if (_session)
            _session->squash();
      }

      void undo() {
         if (_session)
            _session->undo();
      }

      void push() {
         if (_session)
            _session->push();
      }

      maybe_session& operator = ( maybe_session&& mv ) {
         if (mv._session) {
            _session = move(*mv._session);
            mv._session.reset();
         } else {
            _session.reset();
         }

         return *this;
      };

   private:
      optional<database::session>     _session;
};

struct building_block {
   building_block( const block_header_state& prev,
                   block_timestamp_type when,
                   uint16_t num_prev_blocks_to_confirm,
                   const vector<digest_type>& new_protocol_feature_activations )
   :_pending_block_header_state( prev.next( when, num_prev_blocks_to_confirm ) )
   ,_new_protocol_feature_activations( new_protocol_feature_activations )
   {}

   pending_block_header_state         _pending_block_header_state;
   optional<producer_schedule_type>   _new_pending_producer_schedule;
   vector<digest_type>                _new_protocol_feature_activations;
   size_t                             _num_new_protocol_features_that_have_activated = 0;
   vector<transaction_metadata_ptr>   _pending_trx_metas;
   vector<transaction_receipt>        _pending_trx_receipts;
   vector<action_receipt>             _actions;
};

struct assembled_block {
   block_id_type                     _id;
   pending_block_header_state        _pending_block_header_state;
   vector<transaction_metadata_ptr>  _trx_metas;
   signed_block_ptr                  _unsigned_block;
};

struct completed_block {
   block_state_ptr                   _block_state;
};

using block_stage_type = fc::static_variant<building_block, assembled_block, completed_block>;

struct pending_state {
   pending_state( maybe_session&& s, const block_header_state& prev,
                  block_timestamp_type when,
                  uint16_t num_prev_blocks_to_confirm,
                  const vector<digest_type>& new_protocol_feature_activations )
   :_db_session( move(s) )
   ,_block_stage( building_block( prev, when, num_prev_blocks_to_confirm, new_protocol_feature_activations ) )
   {}

   maybe_session                      _db_session;
   block_stage_type                   _block_stage;
   controller::block_status           _block_status = controller::block_status::incomplete;
   optional<block_id_type>            _producer_block_id;

   /** @pre _block_stage cannot hold completed_block alternative */
   const pending_block_header_state& get_pending_block_header_state()const {
      if( _block_stage.contains<building_block>() )
         return _block_stage.get<building_block>()._pending_block_header_state;

      return _block_stage.get<assembled_block>()._pending_block_header_state;
   }

   const vector<transaction_receipt>& get_trx_receipts()const {
      if( _block_stage.contains<building_block>() )
         return _block_stage.get<building_block>()._pending_trx_receipts;

      if( _block_stage.contains<assembled_block>() )
         return _block_stage.get<assembled_block>()._unsigned_block->transactions;

      return _block_stage.get<completed_block>()._block_state->block->transactions;
   }

   const vector<transaction_metadata_ptr>& get_trx_metas()const {
      if( _block_stage.contains<building_block>() )
         return _block_stage.get<building_block>()._pending_trx_metas;

      if( _block_stage.contains<assembled_block>() )
         return _block_stage.get<assembled_block>()._trx_metas;

      return _block_stage.get<completed_block>()._block_state->trxs;
   }

   bool is_protocol_feature_activated( const digest_type& feature_digest )const {
      if( _block_stage.contains<building_block>() ) {
         auto& bb = _block_stage.get<building_block>();
         const auto& activated_features = bb._pending_block_header_state.prev_activated_protocol_features->protocol_features;

         if( activated_features.find( feature_digest ) != activated_features.end() ) return true;

         if( bb._num_new_protocol_features_that_have_activated == 0 ) return false;

         auto end = bb._new_protocol_feature_activations.begin() + bb._num_new_protocol_features_that_have_activated;
         return (std::find( bb._new_protocol_feature_activations.begin(), end, feature_digest ) != end);
      }

      if( _block_stage.contains<assembled_block>() ) {
         // Calling is_protocol_feature_activated during the assembled_block stage is not efficient.
         // We should avoid doing it.
         // In fact for now it isn't even implemented.
         EOS_THROW( misc_exception,
                    "checking if protocol feature is activated in the assembled_block stage is not yet supported" );
         // TODO: implement this
      }

      const auto& activated_features = _block_stage.get<completed_block>()._block_state->activated_protocol_features->protocol_features;
      return (activated_features.find( feature_digest ) != activated_features.end());
   }

   void push() {
      _db_session.push();
   }
};

struct controller_impl {
   controller&                    self;
   chainbase::database            db;
   chainbase::database            reversible_blocks; ///< a special database to persist blocks that have successfully been applied but are still reversible
   block_log                      blog;
   optional<pending_state>        pending;
   block_state_ptr                head;
   fork_database                  fork_db;
   wasm_interface                 wasmif;
   resource_limits_manager        resource_limits;
   authorization_manager          authorization;
   protocol_feature_manager       protocol_features;
   controller::config             conf;
   chain_id_type                  chain_id;
   optional<fc::time_point>       replay_head_time;
   db_read_mode                   read_mode = db_read_mode::SPECULATIVE;
   bool                           in_trx_requiring_checks = false; ///< if true, checks that are normally skipped on replay (e.g. auth checks) cannot be skipped
   optional<fc::microseconds>     subjective_cpu_leeway;
   bool                           trusted_producer_light_validation = false;
   uint32_t                       snapshot_head_block = 0;
   named_thread_pool              thread_pool;

   typedef pair<scope_name,action_name>                   handler_key;
   map< account_name, map<handler_key, apply_handler> >   apply_handlers;
   unordered_map< builtin_protocol_feature_t, std::function<void(controller_impl&)>, enum_hash<builtin_protocol_feature_t> > protocol_feature_activation_handlers;

   /**
    *  Transactions that were undone by pop_block or abort_block, transactions
    *  are removed from this list if they are re-applied in other blocks. Producers
    *  can query this list when scheduling new transactions into blocks.
    */
   unapplied_transactions_type     unapplied_transactions;

   void pop_block() {
      auto prev = fork_db.get_block( head->header.previous );

      if( !prev ) {
         EOS_ASSERT( fork_db.root()->id == head->header.previous, block_validate_exception, "attempt to pop beyond last irreversible block" );
         prev = fork_db.root();
      }

      if( const auto* b = reversible_blocks.find<reversible_block_object,by_num>(head->block_num) )
      {
         reversible_blocks.remove( *b );
      }

      if ( read_mode == db_read_mode::SPECULATIVE ) {
         EOS_ASSERT( head->block, block_validate_exception, "attempting to pop a block that was sparsely loaded from a snapshot");
         for( const auto& t : head->trxs )
            unapplied_transactions[t->signed_id] = t;
      }

      head = prev;
      db.undo();

      protocol_features.popped_blocks_to( prev->block_num );
   }

   template<builtin_protocol_feature_t F>
   void on_activation();

   template<builtin_protocol_feature_t F>
   inline void set_activation_handler() {
      auto res = protocol_feature_activation_handlers.emplace( F, &controller_impl::on_activation<F> );
      EOS_ASSERT( res.second, misc_exception, "attempting to set activation handler twice" );
   }

   inline void trigger_activation_handler( builtin_protocol_feature_t f ) {
      auto itr = protocol_feature_activation_handlers.find( f );
      if( itr == protocol_feature_activation_handlers.end() ) return;
      (itr->second)( *this );
   }

   void set_apply_handler( account_name receiver, account_name contract, action_name action, apply_handler v ) {
      apply_handlers[receiver][make_pair(contract,action)] = v;
   }

   controller_impl( const controller::config& cfg, controller& s, protocol_feature_set&& pfs  )
   :self(s),
    db( cfg.state_dir,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.state_size, false, cfg.db_map_mode, cfg.db_hugepage_paths ),
    reversible_blocks( cfg.blocks_dir/config::reversible_blocks_dir_name,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.reversible_cache_size, false, cfg.db_map_mode, cfg.db_hugepage_paths ),
    blog( cfg.blocks_dir ),
    fork_db( cfg.state_dir ),
    wasmif( cfg.wasm_runtime, db ),
    resource_limits( db ),
    authorization( s, db ),
    protocol_features( std::move(pfs) ),
    conf( cfg ),
    chain_id( cfg.genesis.compute_chain_id() ),
    read_mode( cfg.read_mode ),
    thread_pool( "chain", cfg.thread_pool_size )
   {

      fork_db.open( [this]( block_timestamp_type timestamp,
                            const flat_set<digest_type>& cur_features,
                            const vector<digest_type>& new_features )
                           { check_protocol_features( timestamp, cur_features, new_features ); }
      );

      set_activation_handler<builtin_protocol_feature_t::preactivate_feature>();
      set_activation_handler<builtin_protocol_feature_t::replace_deferred>();
      set_activation_handler<builtin_protocol_feature_t::get_sender>();

      self.irreversible_block.connect([this](const block_state_ptr& bsp) {
         wasmif.current_lib(bsp->block_num);
      });


#define SET_APP_HANDLER( receiver, contract, action) \
   set_apply_handler( #receiver, #contract, #action, &BOOST_PP_CAT(apply_, BOOST_PP_CAT(contract, BOOST_PP_CAT(_,action) ) ) )

   SET_APP_HANDLER( rem, rem, newaccount );
   SET_APP_HANDLER( rem, rem, setcode );
   SET_APP_HANDLER( rem, rem, setabi );
   SET_APP_HANDLER( rem, rem, updateauth );
   SET_APP_HANDLER( rem, rem, deleteauth );
   SET_APP_HANDLER( rem, rem, linkauth );
   SET_APP_HANDLER( rem, rem, unlinkauth );
/*
   SET_APP_HANDLER( rem, rem, postrecovery );
   SET_APP_HANDLER( rem, rem, passrecovery );
   SET_APP_HANDLER( rem, rem, vetorecovery );
*/

   SET_APP_HANDLER( rem, rem, canceldelay );
   }

   /**
    *  Plugins / observers listening to signals emited (such as accepted_transaction) might trigger
    *  errors and throw exceptions. Unless those exceptions are caught it could impact consensus and/or
    *  cause a node to fork.
    *
    *  If it is ever desirable to let a signal handler bubble an exception out of this method
    *  a full audit of its uses needs to be undertaken.
    *
    */
   template<typename Signal, typename Arg>
   void emit( const Signal& s, Arg&& a ) {
      try {
         s( std::forward<Arg>( a ));
      } catch (std::bad_alloc& e) {
         wlog( "std::bad_alloc" );
         throw e;
      } catch (boost::interprocess::bad_alloc& e) {
         wlog( "bad alloc" );
         throw e;
      } catch ( controller_emit_signal_exception& e ) {
         wlog( "${details}", ("details", e.to_detail_string()) );
         throw e;
      } catch ( fc::exception& e ) {
         wlog( "${details}", ("details", e.to_detail_string()) );
      } catch ( ... ) {
         wlog( "signal handler threw exception" );
      }
   }

   void log_irreversible() {
      EOS_ASSERT( fork_db.root(), fork_database_exception, "fork database not properly initialized" );

      const auto& log_head = blog.head();

      auto lib_num = log_head ? log_head->block_num() : (blog.first_block_num() - 1);

      auto root_id = fork_db.root()->id;

      if( log_head ) {
         EOS_ASSERT( root_id == log_head->id(), fork_database_exception, "fork database root does not match block log head" );
      } else {
         EOS_ASSERT( fork_db.root()->block_num == lib_num, fork_database_exception,
                     "empty block log expects the first appended block to build off a block that is not the fork database root" );
      }

      auto fork_head = (read_mode == db_read_mode::IRREVERSIBLE) ? fork_db.pending_head() : fork_db.head();

      if( fork_head->dpos_irreversible_blocknum <= lib_num )
         return;

      const auto branch = fork_db.fetch_branch( fork_head->id, fork_head->dpos_irreversible_blocknum );
      try {
         const auto& rbi = reversible_blocks.get_index<reversible_block_index,by_num>();

         for( auto bitr = branch.rbegin(); bitr != branch.rend(); ++bitr ) {
            if( read_mode == db_read_mode::IRREVERSIBLE ) {
               apply_block( *bitr, controller::block_status::complete );
               head = (*bitr);
               fork_db.mark_valid( head );
            }

            emit( self.irreversible_block, *bitr );

            db.commit( (*bitr)->block_num );
            root_id = (*bitr)->id;

            blog.append( (*bitr)->block );

            auto rbitr = rbi.begin();
            while( rbitr != rbi.end() && rbitr->blocknum <= (*bitr)->block_num ) {
               reversible_blocks.remove( *rbitr );
               rbitr = rbi.begin();
            }
         }
      } catch( fc::exception& ) {
         if( root_id != fork_db.root()->id ) {
            fork_db.advance_root( root_id );
         }
         throw;
      }

      //db.commit( fork_head->dpos_irreversible_blocknum ); // redundant

      if( root_id != fork_db.root()->id ) {
         fork_db.advance_root( root_id );
      }
   }

   /**
    *  Sets fork database head to the genesis state.
    */
   void initialize_blockchain_state() {
      wlog( "Initializing new blockchain with genesis state" );
      producer_schedule_type initial_schedule{ 0, {{config::system_account_name, conf.genesis.initial_key}} };

      block_header_state genheader;
      genheader.active_schedule                = initial_schedule;
      genheader.pending_schedule.schedule      = initial_schedule;
      genheader.pending_schedule.schedule_hash = fc::sha256::hash(initial_schedule);
      genheader.header.timestamp               = conf.genesis.initial_timestamp;
      genheader.header.action_mroot            = conf.genesis.compute_chain_id();
      genheader.id                             = genheader.header.id();
      genheader.block_num                      = genheader.header.block_num();

      head = std::make_shared<block_state>();
      static_cast<block_header_state&>(*head) = genheader;
      head->activated_protocol_features = std::make_shared<protocol_feature_activation_set>();
      head->block = std::make_shared<signed_block>(genheader.header);
      db.set_revision( head->block_num );
      initialize_database();
   }

   void replay(std::function<bool()> shutdown) {
      auto blog_head = blog.head();
      auto blog_head_time = blog_head->timestamp.to_time_point();
      replay_head_time = blog_head_time;
      auto start_block_num = head->block_num + 1;
      auto start = fc::time_point::now();

      std::exception_ptr except_ptr;

      if( start_block_num <= blog_head->block_num() ) {
         ilog( "existing block log, attempting to replay from ${s} to ${n} blocks",
               ("s", start_block_num)("n", blog_head->block_num()) );
         try {
            while( auto next = blog.read_block_by_num( head->block_num + 1 ) ) {
               replay_push_block( next, controller::block_status::irreversible );
               if( next->block_num() % 500 == 0 ) {
                  ilog( "${n} of ${head}", ("n", next->block_num())("head", blog_head->block_num()) );
                  if( shutdown() ) break;
               }
            }
         } catch(  const database_guard_exception& e ) {
            except_ptr = std::current_exception();
         }
         ilog( "${n} irreversible blocks replayed", ("n", 1 + head->block_num - start_block_num) );

         auto pending_head = fork_db.pending_head();
         if( pending_head->block_num < head->block_num || head->block_num < fork_db.root()->block_num ) {
            ilog( "resetting fork database with new last irreversible block as the new root: ${id}",
                  ("id", head->id) );
            fork_db.reset( *head );
         } else if( head->block_num != fork_db.root()->block_num ) {
            auto new_root = fork_db.search_on_branch( pending_head->id, head->block_num );
            EOS_ASSERT( new_root, fork_database_exception, "unexpected error: could not find new LIB in fork database" );
            ilog( "advancing fork database root to new last irreversible block within existing fork database: ${id}",
                  ("id", new_root->id) );
            fork_db.mark_valid( new_root );
            fork_db.advance_root( new_root->id );
         }

         // if the irreverible log is played without undo sessions enabled, we need to sync the
         // revision ordinal to the appropriate expected value here.
         if( self.skip_db_sessions( controller::block_status::irreversible ) )
            db.set_revision( head->block_num );
      } else {
         ilog( "no irreversible blocks need to be replayed" );
      }

      if( !except_ptr && !shutdown() ) {
         int rev = 0;
         while( auto obj = reversible_blocks.find<reversible_block_object,by_num>(head->block_num+1) ) {
            ++rev;
            replay_push_block( obj->get_block(), controller::block_status::validated );
         }
         ilog( "${n} reversible blocks replayed", ("n",rev) );
      }

      auto end = fc::time_point::now();
      ilog( "replayed ${n} blocks in ${duration} seconds, ${mspb} ms/block",
            ("n", head->block_num + 1 - start_block_num)("duration", (end-start).count()/1000000)
            ("mspb", ((end-start).count()/1000.0)/(head->block_num-start_block_num)) );
      replay_head_time.reset();

      if( except_ptr ) {
         std::rethrow_exception( except_ptr );
      }
   }

   void init(std::function<bool()> shutdown, const snapshot_reader_ptr& snapshot) {
      // Setup state if necessary (or in the default case stay with already loaded state):
      uint32_t lib_num = 1u;
      if( snapshot ) {
         snapshot->validate();
         if( blog.head() ) {
            lib_num = blog.head()->block_num();
            read_from_snapshot( snapshot, blog.first_block_num(), lib_num );
         } else {
            read_from_snapshot( snapshot, 0, std::numeric_limits<uint32_t>::max() );
            lib_num = head->block_num;
            blog.reset( conf.genesis, signed_block_ptr(), lib_num + 1 );
         }
      } else {
         if( db.revision() < 1 || !fork_db.head() ) {
            if( fork_db.head() ) {
               if( read_mode == db_read_mode::IRREVERSIBLE && fork_db.head()->id != fork_db.root()->id ) {
                  fork_db.rollback_head_to_root();
               }
               wlog( "No existing chain state. Initializing fresh blockchain state." );
            } else {
               EOS_ASSERT( db.revision() < 1, database_exception,
                           "No existing fork database despite existing chain state. Replay required." );
               wlog( "No existing chain state or fork database. Initializing fresh blockchain state and resetting fork database.");
            }
            initialize_blockchain_state(); // sets head to genesis state

            if( !fork_db.head() ) {
               fork_db.reset( *head );
            }

            if( blog.head() ) {
               EOS_ASSERT( blog.first_block_num() == 1, block_log_exception,
                           "block log does not start with genesis block"
               );
               lib_num = blog.head()->block_num();
            } else {
               blog.reset( conf.genesis, head->block );
            }
         } else {
            lib_num = fork_db.root()->block_num;
            auto first_block_num = blog.first_block_num();
            if( blog.head() ) {
               EOS_ASSERT( first_block_num <= lib_num && lib_num <= blog.head()->block_num(),
                           block_log_exception,
                           "block log does not contain last irreversible block",
                           ("block_log_first_num", first_block_num)
                           ("block_log_last_num", blog.head()->block_num())
                           ("fork_db_lib", lib_num)
               );
               lib_num = blog.head()->block_num();
            } else {
               lib_num = fork_db.root()->block_num;
               if( first_block_num != (lib_num + 1) ) {
                  blog.reset( conf.genesis, signed_block_ptr(), lib_num + 1 );
               }
            }

            if( read_mode == db_read_mode::IRREVERSIBLE && fork_db.head()->id != fork_db.root()->id ) {
               fork_db.rollback_head_to_root();
            }
            head = fork_db.head();
         }
      }
      // At this point head != nullptr && fork_db.head() != nullptr && fork_db.root() != nullptr.
      // Furthermore, fork_db.root()->block_num <= lib_num.
      // Also, even though blog.head() may still be nullptr, blog.first_block_num() is guaranteed to be lib_num + 1.

      EOS_ASSERT( db.revision() >= head->block_num, fork_database_exception,
                  "fork database head is inconsistent with state",
                  ("db",db.revision())("head",head->block_num) );

      if( db.revision() > head->block_num ) {
         wlog( "database revision (${db}) is greater than head block number (${head}), "
               "attempting to undo pending changes",
               ("db",db.revision())("head",head->block_num) );
      }
      while( db.revision() > head->block_num ) {
         db.undo();
      }

      protocol_features.init( db );

      const auto& rbi = reversible_blocks.get_index<reversible_block_index,by_num>();
      auto last_block_num = lib_num;

      if( read_mode == db_read_mode::IRREVERSIBLE ) {
         // ensure there are no reversible blocks
         auto itr = rbi.begin();
         if( itr != rbi.end() ) {
            wlog( "read_mode has changed to irreversible: erasing reversible blocks" );
         }
         for( ; itr != rbi.end(); itr = rbi.begin() )
            reversible_blocks.remove( *itr );
      } else {
         auto itr = rbi.begin();
         for( ; itr != rbi.end() && itr->blocknum <= lib_num; itr = rbi.begin() )
            reversible_blocks.remove( *itr );

         EOS_ASSERT( itr == rbi.end() || itr->blocknum == lib_num + 1, reversible_blocks_exception,
                     "gap exists between last irreversible block and first reversible block",
                     ("lib", lib_num)("first_reversible_block_num", itr->blocknum)
         );

         auto ritr = rbi.rbegin();

         if( ritr != rbi.rend() ) {
            last_block_num = ritr->blocknum;
         }

         EOS_ASSERT( head->block_num <= last_block_num, reversible_blocks_exception,
                     "head block (${head_num}) is greater than the last locally stored block (${last_block_num})",
                     ("head_num", head->block_num)("last_block_num", last_block_num)
         );

         auto pending_head = fork_db.pending_head();

         if( ritr != rbi.rend()
             && lib_num < pending_head->block_num
             && pending_head->block_num <= last_block_num
         ) {
            auto rbitr = rbi.find( pending_head->block_num );
            EOS_ASSERT( rbitr != rbi.end(), reversible_blocks_exception, "pending head block not found in reversible blocks");
            auto rev_id = rbitr->get_block_id();
            EOS_ASSERT( rev_id == pending_head->id,
                        reversible_blocks_exception,
                        "mismatch in block id of pending head block ${num} in reversible blocks database: "
                        "expected: ${expected}, actual: ${actual}",
                        ("num", pending_head->block_num)("expected", pending_head->id)("actual", rev_id)
            );
         } else if( ritr != rbi.rend() && last_block_num < pending_head->block_num ) {
            const auto b = fork_db.search_on_branch( pending_head->id, last_block_num );
            FC_ASSERT( b, "unexpected violation of invariants" );
            auto rev_id = ritr->get_block_id();
            EOS_ASSERT( rev_id == b->id,
                        reversible_blocks_exception,
                        "mismatch in block id of last block (${num}) in reversible blocks database: "
                        "expected: ${expected}, actual: ${actual}",
                        ("num", last_block_num)("expected", b->id)("actual", rev_id)
            );
         }
         // else no checks needed since fork_db will be completely reset on replay anyway
      }

      bool report_integrity_hash = !!snapshot || (lib_num > head->block_num);

      if( last_block_num > head->block_num ) {
         replay( shutdown ); // replay any irreversible and reversible blocks ahead of current head
      }

      if( shutdown() ) return;

      if( read_mode != db_read_mode::IRREVERSIBLE
          && fork_db.pending_head()->id != fork_db.head()->id
          && fork_db.head()->id == fork_db.root()->id
      ) {
         wlog( "read_mode has changed from irreversible: applying best branch from fork database" );

         for( auto pending_head = fork_db.pending_head();
              pending_head->id != fork_db.head()->id;
              pending_head = fork_db.pending_head()
         ) {
            wlog( "applying branch from fork database ending with block: ${id}", ("id", pending_head->id) );
            maybe_switch_forks( pending_head, controller::block_status::complete );
         }
      }

      if( report_integrity_hash ) {
         const auto hash = calculate_integrity_hash();
         ilog( "database initialized with hash: ${hash}", ("hash", hash) );
      }
   }

   ~controller_impl() {
      thread_pool.stop();
      pending.reset();
   }

   void add_indices() {
      reversible_blocks.add_index<reversible_block_index>();

      controller_index_set::add_indices(db);
      contract_database_index_set::add_indices(db);

      authorization.add_indices();
      resource_limits.add_indices();
   }

   void clear_all_undo() {
      // Rewind the database to the last irreversible block
      db.undo_all();
               packed_transactions.emplace_back( std::move( mtrx ) );
            }
         }

         transaction_trace_ptr trace;

         size_t packed_idx = 0;
         for( const auto& receipt : b->transactions ) {
            const auto& trx_receipts = pending->_block_stage.get<building_block>()._pending_trx_receipts;
            auto num_pending_receipts = trx_receipts.size();
            if( receipt.trx.contains<packed_transaction>() ) {
               trace = push_transaction( packed_transactions.at(packed_idx++), fc::time_point::maximum(), receipt.cpu_usage_us, true );
            } else if( receipt.trx.contains<transaction_id_type>() ) {
               trace = push_scheduled_transaction( receipt.trx.get<transaction_id_type>(), fc::time_point::maximum(), receipt.cpu_usage_us, true );
            } else {
               EOS_ASSERT( false, block_validate_exception, "encountered unexpected receipt type" );
            }

            bool transaction_failed =  trace && trace->except;
            bool transaction_can_fail = receipt.status == transaction_receipt_header::hard_fail && receipt.trx.contains<transaction_id_type>();
            if( transaction_failed && !transaction_can_fail) {
               edump((*trace));
               throw *trace->except;
            }

            EOS_ASSERT( trx_receipts.size() > 0,
                        block_validate_exception, "expected a receipt",
                        ("block", *b)("expected_receipt", receipt)
                      );
            EOS_ASSERT( trx_receipts.size() == num_pending_receipts + 1,
                        block_validate_exception, "expected receipt was not added",
                        ("block", *b)("expected_receipt", receipt)
                      );
            const transaction_receipt_header& r = trx_receipts.back();
            EOS_ASSERT( r == static_cast<const transaction_receipt_header&>(receipt),
                        block_validate_exception, "receipt does not match",
                        ("producer_receipt", receipt)("validator_receipt", trx_receipts.back()) );
         }

         finalize_block();

         auto& ab = pending->_block_stage.get<assembled_block>();

         // this implicitly asserts that all header fields (less the signature) are identical
         EOS_ASSERT( producer_block_id == ab._id, block_validate_exception, "Block ID does not match",
                     ("producer_block_id",producer_block_id)("validator_block_id",ab._id) );

         auto bsp = std::make_shared<block_state>(
                        std::move( ab._pending_block_header_state ),
                        b,
                        std::move( ab._trx_metas ),
                        []( block_timestamp_type timestamp,
                            const flat_set<digest_type>& cur_features,
                            const vector<digest_type>& new_features )
                        {}, // validation of any new protocol features should have already occurred prior to apply_block
                        true // signature should have already been verified (assuming untrusted) prior to apply_block
                    );

         pending->_block_stage = completed_block{ bsp };

         commit_block(false);
         return;
      } catch ( const fc::exception& e ) {
         edump((e.to_detail_string()));
         abort_block();
         throw;
      }
   } FC_CAPTURE_AND_RETHROW() } /// apply_block

   std::future<block_state_ptr> create_block_state_future( const signed_block_ptr& b ) {
      EOS_ASSERT( b, block_validate_exception, "null block" );

      auto id = b->id();

      // no reason for a block_state if fork_db already knows about block
      auto existing = fork_db.get_block( id );
      EOS_ASSERT( !existing, fork_database_exception, "we already know about this block: ${id}", ("id", id) );

      auto prev = fork_db.get_block_header( b->previous );
      EOS_ASSERT( prev, unlinkable_block_exception,
                  "unlinkable block ${id}", ("id", id)("previous", b->previous) );

      return async_thread_pool( thread_pool.get_executor(), [b, prev, control=this]() {
         const bool skip_validate_signee = false;
         return std::make_shared<block_state>(
                        *prev,
                        move( b ),
                        [control]( block_timestamp_type timestamp,
                                   const flat_set<digest_type>& cur_features,
                                   const vector<digest_type>& new_features )
                        { control->check_protocol_features( timestamp, cur_features, new_features ); },
                        skip_validate_signee
         );
      } );
   }

   void push_block( std::future<block_state_ptr>& block_state_future ) {
      controller::block_status s = controller::block_status::complete;
      EOS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      auto reset_prod_light_validation = fc::make_scoped_exit([old_value=trusted_producer_light_validation, this]() {
         trusted_producer_light_validation = old_value;
      });
      try {
         block_state_ptr bsp = block_state_future.get();
         const auto& b = bsp->block;

         emit( self.pre_accepted_block, b );

         fork_db.add( bsp );

         if (conf.trusted_producers.count(b->producer)) {
            trusted_producer_light_validation = true;
         };

         emit( self.accepted_block_header, bsp );

         if( read_mode != db_read_mode::IRREVERSIBLE ) {
            maybe_switch_forks( fork_db.pending_head(), s );
         } else {
            log_irreversible();
         }

      } FC_LOG_AND_RETHROW( )
   }

   void replay_push_block( const signed_block_ptr& b, controller::block_status s ) {
      self.validate_db_available_size();
      self.validate_reversible_available_size();

      EOS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      try {
         EOS_ASSERT( b, block_validate_exception, "trying to push empty block" );
         EOS_ASSERT( (s == controller::block_status::irreversible || s == controller::block_status::validated),
                     block_validate_exception, "invalid block status for replay" );
         emit( self.pre_accepted_block, b );
         const bool skip_validate_signee = !conf.force_all_checks;

         auto bsp = std::make_shared<block_state>(
                        *head,
                        b,
                        [this]( block_timestamp_type timestamp,
                                const flat_set<digest_type>& cur_features,
                                const vector<digest_type>& new_features )
                        { check_protocol_features( timestamp, cur_features, new_features ); },
                        skip_validate_signee
         );

         if( s != controller::block_status::irreversible ) {
            fork_db.add( bsp, true );
         }

         emit( self.accepted_block_header, bsp );

         if( s == controller::block_status::irreversible ) {
            apply_block( bsp, s );
            head = bsp;

            // On replay, log_irreversible is not called and so no irreversible_block signal is emittted.
            // So emit it explicitly here.
            emit( self.irreversible_block, bsp );

            if (!self.skip_db_sessions(s)) {
               db.commit(bsp->block_num);
            }

         } else {
            EOS_ASSERT( read_mode != db_read_mode::IRREVERSIBLE, block_validate_exception,
                        "invariant failure: cannot replay reversible blocks while in irreversible mode" );
            maybe_switch_forks( bsp, s );
         }

      } FC_LOG_AND_RETHROW( )
   }

   void maybe_switch_forks( const block_state_ptr& new_head, controller::block_status s ) {
      bool head_changed = true;
      if( new_head->header.previous == head->id ) {
         try {
            apply_block( new_head, s );
            fork_db.mark_valid( new_head );
            head = new_head;
         } catch ( const fc::exception& e ) {
            fork_db.remove( new_head->id );
            throw;
         }
      } else if( new_head->id != head->id ) {
         auto old_head = head;
         ilog("switching forks from ${current_head_id} (block number ${current_head_num}) to ${new_head_id} (block number ${new_head_num})",
              ("current_head_id", head->id)("current_head_num", head->block_num)("new_head_id", new_head->id)("new_head_num", new_head->block_num) );
         auto branches = fork_db.fetch_branch_from( new_head->id, head->id );

         if( branches.second.size() > 0 ) {
            for( auto itr = branches.second.begin(); itr != branches.second.end(); ++itr ) {
               pop_block();
            }
            EOS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                     "loss of sync between fork_db and chainbase during fork switch" ); // _should_ never fail
         }

         for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr ) {
            optional<fc::exception> except;
            try {
               apply_block( *ritr, (*ritr)->is_valid() ? controller::block_status::validated
                                                       : controller::block_status::complete );
               fork_db.mark_valid( *ritr );
               head = *ritr;
            } catch (const fc::exception& e) {
               except = e;
            }
            if( except ) {
               elog("exception thrown while switching forks ${e}", ("e", except->to_detail_string()));

               // ritr currently points to the block that threw
               // Remove the block that threw and all forks built off it.
               fork_db.remove( (*ritr)->id );

               // pop all blocks from the bad fork
               // ritr base is a forward itr to the last block successfully applied
               auto applied_itr = ritr.base();
               for( auto itr = applied_itr; itr != branches.first.end(); ++itr ) {
                  pop_block();
               }
               EOS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                           "loss of sync between fork_db and chainbase during fork switch reversal" ); // _should_ never fail

               // re-apply good blocks
               for( auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr ) {
                  apply_block( *ritr, controller::block_status::validated /* we previously validated these blocks*/ );
                  head = *ritr;
               }
               throw *except;
            } // end if exception
         } /// end for each block in branch

         ilog("successfully switched fork to new head ${new_head_id}", ("new_head_id", new_head->id));
      } else {
         head_changed = false;
      }

      if( head_changed )
         log_irreversible();
   } /// push_block

   void abort_block() {
      if( pending ) {
         if ( read_mode == db_read_mode::SPECULATIVE ) {
            for( const auto& t : pending->get_trx_metas() )
               unapplied_transactions[t->signed_id] = t;
         }
         pending.reset();
         protocol_features.popped_blocks_to( head->block_num );
      }
   }


   bool should_enforce_runtime_limits()const {
      return false;
   }

   checksum256_type calculate_action_merkle() {
      vector<digest_type> action_digests;
      const auto& actions = pending->_block_stage.get<building_block>()._actions;
      action_digests.reserve( actions.size() );
      for( const auto& a : actions )
         action_digests.emplace_back( a.digest() );

      return merkle( move(action_digests) );
   }

   checksum256_type calculate_trx_merkle() {
      vector<digest_type> trx_digests;
      const auto& trxs = pending->_block_stage.get<building_block>()._pending_trx_receipts;
      trx_digests.reserve( trxs.size() );
      for( const auto& a : trxs )
         trx_digests.emplace_back( a.digest() );

      return merkle( move(trx_digests) );
   }

   void update_producers_authority() {
      const auto& producers = pending->get_pending_block_header_state().active_schedule.producers;

      auto update_permission = [&]( auto& permission, auto threshold ) {
         auto auth = authority( threshold, {}, {});
         for( auto& p : producers ) {
            auth.accounts.push_back({{p.producer_name, config::active_name}, 1});
         }

         if( static_cast<authority>(permission.auth) != auth ) { // TODO: use a more efficient way to check that authority has not changed
            db.modify(permission, [&]( auto& po ) {
               po.auth = auth;
            });
         }
      };

      uint32_t num_producers = producers.size();
      auto calculate_threshold = [=]( uint32_t numerator, uint32_t denominator ) {
         return ( (num_producers * numerator) / denominator ) + 1;
      };

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::active_name}),
                         calculate_threshold( 2, 3 ) /* more than two-thirds */                      );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::majority_producers_permission_name}),
                         calculate_threshold( 1, 2 ) /* more than one-half */                        );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::minority_producers_permission_name}),
                         calculate_threshold( 1, 3 ) /* more than one-third */                       );

      //TODO: Add tests
   }

   void create_block_summary(const block_id_type& id) {
      auto block_num = block_header::num_from_id(id);
      auto sid = block_num & 0xffff;
      db.modify( db.get<block_summary_object,by_id>(sid), [&](block_summary_object& bso ) {
          bso.block_id = id;
      });
   }


   void clear_expired_input_transactions() {
      //Look for expired transactions in the deduplication list, and remove them.
      auto& transaction_idx = db.get_mutable_index<transaction_multi_index>();
      const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
      auto now = self.pending_block_time();
      while( (!dedupe_index.empty()) && ( now > fc::time_point(dedupe_index.begin()->expiration) ) ) {
         transaction_idx.remove(*dedupe_index.begin());
      }
   }

   bool sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
      if( conf.sender_bypass_whiteblacklist.size() > 0 &&
          ( conf.sender_bypass_whiteblacklist.find( sender ) != conf.sender_bypass_whiteblacklist.end() ) )
      {
         return true;
      }

      return false;
   }

   void check_actor_list( const flat_set<account_name>& actors )const {
      if( actors.size() == 0 ) return;

      if( conf.actor_whitelist.size() > 0 ) {
         // throw if actors is not a subset of whitelist
         const auto& whitelist = conf.actor_whitelist;
         bool is_subset = true;

         // quick extents check, then brute force the check actors
         if (*actors.cbegin() >= *whitelist.cbegin() && *actors.crbegin() <= *whitelist.crbegin() ) {
            auto lower_bound = whitelist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, whitelist.cend(), actor);

               // if the actor is not found, this is not a subset
               if (lower_bound == whitelist.cend() || *lower_bound != actor ) {
                  is_subset = false;
                  break;
               }

               // if the actor was found, we are guaranteed that other actors are either not present in the whitelist
               // or will be present in the range defined as [next actor,end)
               lower_bound = std::next(lower_bound);
            }
         } else {
            is_subset = false;
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_missing_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& whitelist) -> vector<account_name> {
            vector<account_name> excluded;
            excluded.reserve( actors.size() );
            set_difference( actors.begin(), actors.end(),
                            whitelist.begin(), whitelist.end(),
                            std::back_inserter(excluded) );
            return excluded;
         };

         EOS_ASSERT( is_subset,  actor_whitelist_exception,
                     "authorizing actor(s) in transaction are not on the actor whitelist: ${actors}",
                     ("actors", generate_missing_actors(actors, whitelist))
                   );
      } else if( conf.actor_blacklist.size() > 0 ) {
         // throw if actors intersects blacklist
         const auto& blacklist = conf.actor_blacklist;
         bool intersects = false;

         // quick extents check then brute force check actors
         if( *actors.cbegin() <= *blacklist.crbegin() && *actors.crbegin() >= *blacklist.cbegin() ) {
            auto lower_bound = blacklist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, blacklist.cend(), actor);

               // if the lower bound in the blacklist is at the end, all other actors are guaranteed to
               // not exist in the blacklist
               if (lower_bound == blacklist.cend()) {
                  break;
               }

               // if the lower bound of an actor IS the actor, then we have an intersection
               if (*lower_bound == actor) {
                  intersects = true;
                  break;
               }
            }
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_blacklisted_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& blacklist) -> vector<account_name> {
            vector<account_name> blacklisted;
            blacklisted.reserve( actors.size() );
            set_intersection( actors.begin(), actors.end(),
                              blacklist.begin(), blacklist.end(),
                              std::back_inserter(blacklisted)
                            );
            return blacklisted;
         };

         EOS_ASSERT( !intersects, actor_blacklist_exception,
                     "authorizing actor(s) in transaction are on the actor blacklist: ${actors}",
                     ("actors", generate_blacklisted_actors(actors, blacklist))
                   );
      }
   }

   void check_contract_list( account_name code )const {
      if( conf.contract_whitelist.size() > 0 ) {
         EOS_ASSERT( conf.contract_whitelist.find( code ) != conf.contract_whitelist.end(),
                     contract_whitelist_exception,
                     "account '${code}' is not on the contract whitelist", ("code", code)
                   );
      } else if( conf.contract_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.contract_blacklist.find( code ) == conf.contract_blacklist.end(),
                     contract_blacklist_exception,
                     "account '${code}' is on the contract blacklist", ("code", code)
                   );
      }
   }

   void check_action_list( account_name code, action_name action )const {
      if( conf.action_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.action_blacklist.find( std::make_pair(code, action) ) == conf.action_blacklist.end(),
                     action_blacklist_exception,
                     "action '${code}::${action}' is on the action blacklist",
                     ("code", code)("action", action)
                   );
      }
   }

   void check_key_list( const public_key_type& key )const {
      if( conf.key_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.key_blacklist.find( key ) == conf.key_blacklist.end(),
                     key_blacklist_exception,
                     "public key '${key}' is on the key blacklist",
                     ("key", key)
                   );
      }
   }

   /*
   bool should_check_tapos()const { return true; }

   void validate_tapos( const transaction& trx )const {
      if( !should_check_tapos() ) return;

      const auto& tapos_block_summary = db.get<block_summary_object>((uint16_t)trx.ref_block_num);

      //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
      EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
                 "Transaction's reference block did not match. Is this transaction from a different fork?",
                 ("tapos_summary", tapos_block_summary));
   }
   */


   /**
    *  At the start of each block we notify the system contract with a transaction that passes in
    *  the block header of the prior block (which is currently our head block)
    */
   signed_transaction get_on_block_transaction()
   {
      action on_block_act;
      on_block_act.account = config::system_account_name;
      on_block_act.name = N(onblock);
      on_block_act.authorization = vector<permission_level>{{config::system_account_name, config::active_name}};
      on_block_act.data = fc::raw::pack(self.head_block_header());

      signed_transaction trx;
      trx.actions.emplace_back(std::move(on_block_act));
      if( self.is_builtin_activated( builtin_protocol_feature_t::no_duplicate_deferred_id ) ) {
         trx.expiration = time_point_sec();
         trx.ref_block_num = 0;
         trx.ref_block_prefix = 0;
      } else {
         trx.expiration = self.pending_block_time() + fc::microseconds(999'999); // Round up to nearest second to avoid appearing expired
         trx.set_reference_block( self.head_block_id() );
      }
      return trx;
   }

}; /// controller_impl

const resource_limits_manager&   controller::get_resource_limits_manager()const
{
   return my->resource_limits;
}
resource_limits_manager&         controller::get_mutable_resource_limits_manager()
{
   return my->resource_limits;
}

const authorization_manager&   controller::get_authorization_manager()const
{
   return my->authorization;
}
authorization_manager&         controller::get_mutable_authorization_manager()
{
   return my->authorization;
}

const protocol_feature_manager& controller::get_protocol_feature_manager()const
{
   return my->protocol_features;
}

controller::controller( const controller::config& cfg )
:my( new controller_impl( cfg, *this, protocol_feature_set{} ) )
{
}

controller::controller( const config& cfg, protocol_feature_set&& pfs )
:my( new controller_impl( cfg, *this, std::move(pfs) ) )
{
}

controller::~controller() {
   my->abort_block();
   /* Shouldn't be needed anymore.
   //close fork_db here, because it can generate "irreversible" signal to this controller,
   //in case if read-mode == IRREVERSIBLE, we will apply latest irreversible block
   //for that we need 'my' to be valid pointer pointing to valid controller_impl.
   my->fork_db.close();
   */
}

void controller::add_indices() {
   my->add_indices();
}

void controller::startup( std::function<bool()> shutdown, const snapshot_reader_ptr& snapshot ) {
   if( snapshot ) {
      ilog( "Starting initialization from snapshot, this may take a significant amount of time" );
   }
   try {
      my->init(shutdown, snapshot);
   } catch (boost::interprocess::bad_alloc& e) {
      if ( snapshot )
         elog( "db storage not configured to have enough storage for the provided snapshot, please increase and retry snapshot" );
      throw e;
   }
   if( snapshot ) {
      ilog( "Finished initialization from snapshot" );
   }
}

const chainbase::database& controller::db()const { return my->db; }

chainbase::database& controller::mutable_db()const { return my->db; }

const fork_database& controller::fork_db()const { return my->fork_db; }

void controller::preactivate_feature( const digest_type& feature_digest ) {
   const auto& pfs = my->protocol_features.get_protocol_feature_set();
   auto cur_time = pending_block_time();

   auto status = pfs.is_recognized( feature_digest, cur_time );
   switch( status ) {
      case protocol_feature_set::recognized_t::unrecognized:
         if( is_producing_block() ) {
            EOS_THROW( subjective_block_production_exception,
                       "protocol feature with digest '${digest}' is unrecognized", ("digest", feature_digest) );
         } else {
            EOS_THROW( protocol_feature_bad_block_exception,
                       "protocol feature with digest '${digest}' is unrecognized", ("digest", feature_digest) );
         }
      break;
      case protocol_feature_set::recognized_t::disabled:
         if( is_producing_block() ) {
            EOS_THROW( subjective_block_production_exception,
                       "protocol feature with digest '${digest}' is disabled", ("digest", feature_digest) );
         } else {
            EOS_THROW( protocol_feature_bad_block_exception,
                       "protocol feature with digest '${digest}' is disabled", ("digest", feature_digest) );
         }
      break;
      case protocol_feature_set::recognized_t::too_early:
         if( is_producing_block() ) {
            EOS_THROW( subjective_block_production_exception,
                       "${timestamp} is too early for the earliest allowed activation time of the protocol feature with digest '${digest}'", ("digest", feature_digest)("timestamp", cur_time) );
         } else {
            EOS_THROW( protocol_feature_bad_block_exception,
                       "${timestamp} is too early for the earliest allowed activation time of the protocol feature with digest '${digest}'", ("digest", feature_digest)("timestamp", cur_time) );
         }
      break;
      case protocol_feature_set::recognized_t::ready:
      break;
      default:
         if( is_producing_block() ) {
            EOS_THROW( subjective_block_production_exception, "unexpected recognized_t status" );
         } else {
            EOS_THROW( protocol_feature_bad_block_exception, "unexpected recognized_t status" );
         }
      break;
   }

   // The above failures depend on subjective information.
   // Because of deferred transactions, this complicates things considerably.

   // If producing a block, we throw a subjective failure if the feature is not properly recognized in order
   // to try to avoid retiring into a block a deferred transacton driven by subjective information.

   // But it is still possible for a producer to retire a deferred transaction that deals with this subjective
   // information. If they recognized the feature, they would retire it successfully, but a validator that
   // does not recognize the feature should reject the entire block (not just fail the deferred transaction).
   // Even if they don't recognize the feature, the producer could change their remnode code to treat it like an
   // objective failure thus leading the deferred transaction to retire with soft_fail or hard_fail.
   // In this case, validators that don't recognize the feature would reject the whole block immediately, and
   // validators that do recognize the feature would likely lead to a different retire status which would
   // ultimately cause a validation failure and thus rejection of the block.
   // In either case, it results in rejection of the block which is the desired behavior in this scenario.

   // If the feature is properly recognized by producer and validator, we have dealt with the subjectivity and
   // now only consider the remaining failure modes which are deterministic and objective.
   // Thus the exceptions that can be thrown below can be regular objective exceptions
   // that do not cause immediate rejection of the block.

   EOS_ASSERT( !is_protocol_feature_activated( feature_digest ),
               protocol_feature_exception,
               "protocol feature with digest '${digest}' is already activated",
               ("digest", feature_digest)
   );

   const auto& pso = my->db.get<protocol_state_object>();

   EOS_ASSERT( std::find( pso.preactivated_protocol_features.begin(),
                          pso.preactivated_protocol_features.end(),
                          feature_digest
               ) == pso.preactivated_protocol_features.end(),
               protocol_feature_exception,
               "protocol feature with digest '${digest}' is already pre-activated",
               ("digest", feature_digest)
   );

   auto dependency_checker = [&]( const digest_type& d ) -> bool
   {
      if( is_protocol_feature_activated( d ) ) return true;

      return ( std::find( pso.preactivated_protocol_features.begin(),
                          pso.preactivated_protocol_features.end(),
                          d ) != pso.preactivated_protocol_features.end() );
   };

   EOS_ASSERT( pfs.validate_dependencies( feature_digest, dependency_checker ),
               protocol_feature_exception,
               "not all dependencies of protocol feature with digest '${digest}' have been activated or pre-activated",
               ("digest", feature_digest)
   );

   my->db.modify( pso, [&]( auto& ps ) {
      ps.preactivated_protocol_features.push_back( feature_digest );
   } );
}

vector<digest_type> controller::get_preactivated_protocol_features()const {
   const auto& pso = my->db.get<protocol_state_object>();

   if( pso.preactivated_protocol_features.size() == 0 ) return {};

   vector<digest_type> preactivated_protocol_features;

   for( const auto& f : pso.preactivated_protocol_features ) {
      preactivated_protocol_features.emplace_back( f );
   }

   return preactivated_protocol_features;
}

void controller::validate_protocol_features( const vector<digest_type>& features_to_activate )const {
   my->check_protocol_features( my->head->header.timestamp,
                                my->head->activated_protocol_features->protocol_features,
                                features_to_activate );
}

void controller::start_block( block_timestamp_type when, uint16_t confirm_block_count )
{
   validate_db_available_size();

   EOS_ASSERT( !my->pending, block_validate_exception, "pending block already exists" );

   vector<digest_type> new_protocol_feature_activations;

   const auto& pso = my->db.get<protocol_state_object>();
   if( pso.preactivated_protocol_features.size() > 0 ) {
      for( const auto& f : pso.preactivated_protocol_features ) {
         new_protocol_feature_activations.emplace_back( f );
      }
   }

   if( new_protocol_feature_activations.size() > 0 ) {
      validate_protocol_features( new_protocol_feature_activations );
   }

   my->start_block( when, confirm_block_count, new_protocol_feature_activations,
                    block_status::incomplete, optional<block_id_type>() );
}

void controller::start_block( block_timestamp_type when,
                              uint16_t confirm_block_count,
                              const vector<digest_type>& new_protocol_feature_activations )
{
   validate_db_available_size();

   if( new_protocol_feature_activations.size() > 0 ) {
      validate_protocol_features( new_protocol_feature_activations );
   }

   my->start_block( when, confirm_block_count, new_protocol_feature_activations,
                    block_status::incomplete, optional<block_id_type>() );
}

block_state_ptr controller::finalize_block( const std::function<signature_type( const digest_type& )>& signer_callback ) {
   validate_db_available_size();

   my->finalize_block();

   auto& ab = my->pending->_block_stage.get<assembled_block>();

   auto bsp = std::make_shared<block_state>(
                  std::move( ab._pending_block_header_state ),
                  std::move( ab._unsigned_block ),
                  std::move( ab._trx_metas ),
                  []( block_timestamp_type timestamp,
                      const flat_set<digest_type>& cur_features,
                      const vector<digest_type>& new_features )
                  {},
                  signer_callback
              );

   my->pending->_block_stage = completed_block{ bsp };

   return bsp;
}

void controller::commit_block() {
   validate_db_available_size();
   validate_reversible_available_size();
   my->commit_block(true);
}

void controller::abort_block() {
   my->abort_block();
}

boost::asio::io_context& controller::get_thread_pool() {
   return my->thread_pool.get_executor();
}

std::future<block_state_ptr> controller::create_block_state_future( const signed_block_ptr& b ) {
   return my->create_block_state_future( b );
}

void controller::push_block( std::future<block_state_ptr>& block_state_future ) {
   validate_db_available_size();
   validate_reversible_available_size();
   my->push_block( block_state_future );
}

transaction_trace_ptr controller::push_transaction( const transaction_metadata_ptr& trx, fc::time_point deadline, uint32_t billed_cpu_time_us ) {
   validate_db_available_size();
   EOS_ASSERT( get_read_mode() != chain::db_read_mode::READ_ONLY, transaction_type_exception, "push transaction not allowed in read-only mode" );
   EOS_ASSERT( trx && !trx->implicit && !trx->scheduled, transaction_type_exception, "Implicit/Scheduled transaction not allowed" );
   return my->push_transaction(trx, deadline, billed_cpu_time_us, billed_cpu_time_us > 0 );
}

transaction_trace_ptr controller::push_scheduled_transaction( const transaction_id_type& trxid, fc::time_point deadline, uint32_t billed_cpu_time_us )
{
   validate_db_available_size();
   return my->push_scheduled_transaction( trxid, deadline, billed_cpu_time_us, billed_cpu_time_us > 0 );
}

const flat_set<account_name>& controller::get_actor_whitelist() const {
   return my->conf.actor_whitelist;
}
const flat_set<account_name>& controller::get_actor_blacklist() const {
   return my->conf.actor_blacklist;
}
const flat_set<account_name>& controller::get_contract_whitelist() const {
   return my->conf.contract_whitelist;
}
const flat_set<account_name>& controller::get_contract_blacklist() const {
   return my->conf.contract_blacklist;
}
const flat_set< pair<account_name, action_name> >& controller::get_action_blacklist() const {
   return my->conf.action_blacklist;
}
const flat_set<public_key_type>& controller::get_key_blacklist() const {
   return my->conf.key_blacklist;
}

void controller::set_actor_whitelist( const flat_set<account_name>& new_actor_whitelist ) {
   my->conf.actor_whitelist = new_actor_whitelist;
}
void controller::set_actor_blacklist( const flat_set<account_name>& new_actor_blacklist ) {
   my->conf.actor_blacklist = new_actor_blacklist;
}
void controller::set_contract_whitelist( const flat_set<account_name>& new_contract_whitelist ) {
   my->conf.contract_whitelist = new_contract_whitelist;
}
void controller::set_contract_blacklist( const flat_set<account_name>& new_contract_blacklist ) {
   my->conf.contract_blacklist = new_contract_blacklist;
}
void controller::set_action_blacklist( const flat_set< pair<account_name, action_name> >& new_action_blacklist ) {
   for (auto& act: new_action_blacklist) {
      EOS_ASSERT(act.first != account_name(), name_type_exception, "Action blacklist - contract name should not be empty");
      EOS_ASSERT(act.second != action_name(), action_type_exception, "Action blacklist - action name should not be empty");
   }
   my->conf.action_blacklist = new_action_blacklist;
}
void controller::set_key_blacklist( const flat_set<public_key_type>& new_key_blacklist ) {
   my->conf.key_blacklist = new_key_blacklist;
}

uint32_t controller::head_block_num()const {
   return my->head->block_num;
}
time_point controller::head_block_time()const {
   return my->head->header.timestamp;
}
block_id_type controller::head_block_id()const {
   return my->head->id;
}
account_name  controller::head_block_producer()const {
   return my->head->header.producer;
}
const block_header& controller::head_block_header()const {
   return my->head->header;
}
block_state_ptr controller::head_block_state()const {
   return my->head;
}

uint32_t controller::fork_db_head_block_num()const {
   return my->fork_db.head()->block_num;
}

block_id_type controller::fork_db_head_block_id()const {
   return my->fork_db.head()->id;
}

time_point controller::fork_db_head_block_time()const {
   return my->fork_db.head()->header.timestamp;
}

account_name  controller::fork_db_head_block_producer()const {
   return my->fork_db.head()->header.producer;
}

uint32_t controller::fork_db_pending_head_block_num()const {
   return my->fork_db.pending_head()->block_num;
}

block_id_type controller::fork_db_pending_head_block_id()const {
   return my->fork_db.pending_head()->id;
}

time_point controller::fork_db_pending_head_block_time()const {
   return my->fork_db.pending_head()->header.timestamp;
}

account_name  controller::fork_db_pending_head_block_producer()const {
   return my->fork_db.pending_head()->header.producer;
}

time_point controller::pending_block_time()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( my->pending->_block_stage.contains<completed_block>() )
      return my->pending->_block_stage.get<completed_block>()._block_state->header.timestamp;

   return my->pending->get_pending_block_header_state().timestamp;
}

account_name controller::pending_block_producer()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( my->pending->_block_stage.contains<completed_block>() )
      return my->pending->_block_stage.get<completed_block>()._block_state->header.producer;

   return my->pending->get_pending_block_header_state().producer;
}

public_key_type controller::pending_block_signing_key()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );

   if( my->pending->_block_stage.contains<completed_block>() )
      return my->pending->_block_stage.get<completed_block>()._block_state->block_signing_key;

   return my->pending->get_pending_block_header_state().block_signing_key;
}

optional<block_id_type> controller::pending_producer_block_id()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   return my->pending->_producer_block_id;
}

const vector<transaction_receipt>& controller::get_pending_trx_receipts()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   return my->pending->get_trx_receipts();
}

uint32_t controller::last_irreversible_block_num() const {
   return my->fork_db.root()->block_num;
}

block_id_type controller::last_irreversible_block_id() const {
   auto lib_num = last_irreversible_block_num();
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)lib_num);

   if( block_header::num_from_id(tapos_block_summary.block_id) == lib_num )
      return tapos_block_summary.block_id;

   auto signed_blk = my->blog.read_block_by_num( lib_num );

   EOS_ASSERT( BOOST_LIKELY( signed_blk != nullptr ), unknown_block_exception,
               "Could not find block: ${block}", ("block", lib_num) );

   return signed_blk->id();
}

const dynamic_global_property_object& controller::get_dynamic_global_properties()const {
  return my->db.get<dynamic_global_property_object>();
}
const global_property_object& controller::get_global_properties()const {
  return my->db.get<global_property_object>();
}

signed_block_ptr controller::fetch_block_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   if( state && state->block ) return state->block;
   auto bptr = fetch_block_by_number( block_header::num_from_id(id) );
   if( bptr && bptr->id() == id ) return bptr;
   return signed_block_ptr();
}

signed_block_ptr controller::fetch_block_by_number( uint32_t block_num )const  { try {
   auto blk_state = fetch_block_state_by_number( block_num );
   if( blk_state ) {
      return blk_state->block;
   }

   return my->blog.read_block_by_num(block_num);
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_state_ptr controller::fetch_block_state_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   return state;
}

block_state_ptr controller::fetch_block_state_by_number( uint32_t block_num )const  { try {
   const auto& rev_blocks = my->reversible_blocks.get_index<reversible_block_index,by_num>();
   auto objitr = rev_blocks.find(block_num);

   if( objitr == rev_blocks.end() ) {
      if( my->read_mode == db_read_mode::IRREVERSIBLE ) {
         return my->fork_db.search_on_branch( my->fork_db.pending_head()->id, block_num );
      } else {
         return block_state_ptr();
      }
   }

   return my->fork_db.get_block( objitr->get_block_id() );
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_id_type controller::get_block_id_for_num( uint32_t block_num )const { try {
   const auto& blog_head = my->blog.head();

   bool find_in_blog = (blog_head && block_num <= blog_head->block_num());

   if( !find_in_blog ) {
      if( my->read_mode != db_read_mode::IRREVERSIBLE ) {
         const auto& rev_blocks = my->reversible_blocks.get_index<reversible_block_index,by_num>();
         auto objitr = rev_blocks.find(block_num);
         if( objitr != rev_blocks.end() ) {
            return objitr->get_block_id();
         }
      } else {
         auto bsp = my->fork_db.search_on_branch( my->fork_db.pending_head()->id, block_num );

         if( bsp ) return bsp->id;
      }
   }

   auto signed_blk = my->blog.read_block_by_num(block_num);

   EOS_ASSERT( BOOST_LIKELY( signed_blk != nullptr ), unknown_block_exception,
               "Could not find block: ${block}", ("block", block_num) );

   return signed_blk->id();
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

sha256 controller::calculate_integrity_hash()const { try {
   return my->calculate_integrity_hash();
} FC_LOG_AND_RETHROW() }

void controller::write_snapshot( const snapshot_writer_ptr& snapshot ) const {
   EOS_ASSERT( !my->pending, block_validate_exception, "cannot take a consistent snapshot with a pending block" );
   return my->add_to_snapshot(snapshot);
}

void controller::pop_block() {
   my->pop_block();
}

int64_t controller::set_proposed_producers( vector<producer_key> producers ) {
   const auto& gpo = get_global_properties();
   auto cur_block_num = head_block_num() + 1;

   if( producers.size() == 0 && is_builtin_activated( builtin_protocol_feature_t::disallow_empty_producer_schedule ) ) {
      return -1;
   }

   if( gpo.proposed_schedule_block_num.valid() ) {
      if( *gpo.proposed_schedule_block_num != cur_block_num )
         return -1; // there is already a proposed schedule set in a previous block, wait for it to become pending

      if( std::equal( producers.begin(), producers.end(),
                      gpo.proposed_schedule.producers.begin(), gpo.proposed_schedule.producers.end() ) )
         return -1; // the proposed producer schedule does not change
   }

   producer_schedule_type sch;

   decltype(sch.producers.cend()) end;
   decltype(end)                  begin;

   const auto& pending_sch = pending_producers();

   if( pending_sch.producers.size() == 0 ) {
      const auto& active_sch = active_producers();
      begin = active_sch.producers.begin();
      end   = active_sch.producers.end();
      sch.version = active_sch.version + 1;
   } else {
      begin = pending_sch.producers.begin();
      end   = pending_sch.producers.end();
      sch.version = pending_sch.version + 1;
   }

   if( std::equal( producers.begin(), producers.end(), begin, end ) )
      return -1; // the producer schedule would not change

   sch.producers = std::move(producers);

   int64_t version = sch.version;

   ilog( "proposed producer schedule with version ${v}", ("v", version) );

   my->db.modify( gpo, [&]( auto& gp ) {
      gp.proposed_schedule_block_num = cur_block_num;
      gp.proposed_schedule = std::move(sch);
   });
   return version;
}

const producer_schedule_type&    controller::active_producers()const {
   if( !(my->pending) )
      return  my->head->active_schedule;

   if( my->pending->_block_stage.contains<completed_block>() )
      return my->pending->_block_stage.get<completed_block>()._block_state->active_schedule;

   return my->pending->get_pending_block_header_state().active_schedule;
}

const producer_schedule_type&    controller::pending_producers()const {
   if( !(my->pending) )
      return  my->head->pending_schedule.schedule;

   if( my->pending->_block_stage.contains<completed_block>() )
      return my->pending->_block_stage.get<completed_block>()._block_state->pending_schedule.schedule;

   if( my->pending->_block_stage.contains<assembled_block>() ) {
      const auto& np = my->pending->_block_stage.get<assembled_block>()._unsigned_block->new_producers;
      if( np )
         return *np;
   }

   const auto& bb = my->pending->_block_stage.get<building_block>();

   if( bb._new_pending_producer_schedule )
      return *bb._new_pending_producer_schedule;

   return bb._pending_block_header_state.prev_pending_schedule.schedule;
}

optional<producer_schedule_type> controller::proposed_producers()const {
   const auto& gpo = get_global_properties();
   if( !gpo.proposed_schedule_block_num.valid() )
      return optional<producer_schedule_type>();

   return gpo.proposed_schedule;
}

bool controller::light_validation_allowed(bool replay_opts_disabled_by_policy) const {
   if (!my->pending || my->in_trx_requiring_checks) {
      return false;
   }

   const auto pb_status = my->pending->_block_status;

   // in a pending irreversible or previously validated block and we have forcing all checks
   const bool consider_skipping_on_replay = (pb_status == block_status::irreversible || pb_status == block_status::validated) && !replay_opts_disabled_by_policy;

   // OR in a signed block and in light validation mode
   const bool consider_skipping_on_validate = (pb_status == block_status::complete &&
         (my->conf.block_validation_mode == validation_mode::LIGHT || my->trusted_producer_light_validation));

   return consider_skipping_on_replay || consider_skipping_on_validate;
}


bool controller::skip_auth_check() const {
   return light_validation_allowed(my->conf.force_all_checks);
}

bool controller::skip_db_sessions( block_status bs ) const {
   bool consider_skipping = bs == block_status::irreversible;
   return consider_skipping
      && !my->conf.disable_replay_opts
      && !my->in_trx_requiring_checks;
}

bool controller::skip_db_sessions( ) const {
   if (my->pending) {
      return skip_db_sessions(my->pending->_block_status);
   } else {
      return false;
   }
}

bool controller::skip_trx_checks() const {
   return light_validation_allowed(my->conf.disable_replay_opts);
}

bool controller::contracts_console()const {
   return my->conf.contracts_console;
}

chain_id_type controller::get_chain_id()const {
   return my->chain_id;
}

db_read_mode controller::get_read_mode()const {
   return my->read_mode;
}

validation_mode controller::get_validation_mode()const {
   return my->conf.block_validation_mode;
}

const apply_handler* controller::find_apply_handler( account_name receiver, account_name scope, action_name act ) const
{
   auto native_handler_scope = my->apply_handlers.find( receiver );
   if( native_handler_scope != my->apply_handlers.end() ) {
      auto handler = native_handler_scope->second.find( make_pair( scope, act ) );
      if( handler != native_handler_scope->second.end() )
         return &handler->second;
   }
   return nullptr;
}
wasm_interface& controller::get_wasm_interface() {
   return my->wasmif;
}

const account_object& controller::get_account( account_name name )const
{ try {
   return my->db.get<account_object, by_name>(name);
} FC_CAPTURE_AND_RETHROW( (name) ) }

unapplied_transactions_type& controller::get_unapplied_transactions() {
   if ( my->read_mode != db_read_mode::SPECULATIVE ) {
      EOS_ASSERT( my->unapplied_transactions.empty(), transaction_exception,
                  "not empty unapplied_transactions in non-speculative mode" ); //should never happen
   }
   return my->unapplied_transactions;
}

bool controller::sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
   return my->sender_avoids_whitelist_blacklist_enforcement( sender );
}

void controller::check_actor_list( const flat_set<account_name>& actors )const {
   my->check_actor_list( actors );
}

void controller::check_contract_list( account_name code )const {
   my->check_contract_list( code );
}

void controller::check_action_list( account_name code, action_name action )const {
   my->check_action_list( code, action );
}

void controller::check_key_list( const public_key_type& key )const {
   my->check_key_list( key );
}

bool controller::is_building_block()const {
   return my->pending.valid();
}

bool controller::is_producing_block()const {
   if( !my->pending ) return false;

   return (my->pending->_block_status == block_status::incomplete);
}

bool controller::is_ram_billing_in_notify_allowed()const {
   return my->conf.disable_all_subjective_mitigations || !is_producing_block() || my->conf.allow_ram_billing_in_notify;
}

void controller::validate_expiration( const transaction& trx )const { try {
   const auto& chain_configuration = get_global_properties().configuration;

   EOS_ASSERT( time_point(trx.expiration) >= pending_block_time(),
               expired_tx_exception,
               "transaction has expired, "
               "expiration is ${trx.expiration} and pending block time is ${pending_block_time}",
               ("trx.expiration",trx.expiration)("pending_block_time",pending_block_time()));
   EOS_ASSERT( time_point(trx.expiration) <= pending_block_time() + fc::seconds(chain_configuration.max_transaction_lifetime),
               tx_exp_too_far_exception,
               "Transaction expiration is too far in the future relative to the reference time of ${reference_time}, "
               "expiration is ${trx.expiration} and the maximum transaction lifetime is ${max_til_exp} seconds",
               ("trx.expiration",trx.expiration)("reference_time",pending_block_time())
               ("max_til_exp",chain_configuration.max_transaction_lifetime) );
} FC_CAPTURE_AND_RETHROW((trx)) }

void controller::validate_tapos( const transaction& trx )const { try {
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)trx.ref_block_num);

   //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
   EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
              "Transaction's reference block did not match. Is this transaction from a different fork?",
              ("tapos_summary", tapos_block_summary));
} FC_CAPTURE_AND_RETHROW() }

void controller::validate_db_available_size() const {
   const auto free = db().get_segment_manager()->get_free_memory();
   const auto guard = my->conf.state_guard_size;
   EOS_ASSERT(free >= guard, database_guard_exception, "database free: ${f}, guard size: ${g}", ("f", free)("g",guard));
}

void controller::validate_reversible_available_size() const {
   const auto free = my->reversible_blocks.get_segment_manager()->get_free_memory();
   const auto guard = my->conf.reversible_guard_size;
   EOS_ASSERT(free >= guard, reversible_guard_exception, "reversible free: ${f}, guard size: ${g}", ("f", free)("g",guard));
}

bool controller::is_protocol_feature_activated( const digest_type& feature_digest )const {
   if( my->pending )
      return my->pending->is_protocol_feature_activated( feature_digest );

   const auto& activated_features = my->head->activated_protocol_features->protocol_features;
   return (activated_features.find( feature_digest ) != activated_features.end());
}

bool controller::is_builtin_activated( builtin_protocol_feature_t f )const {
   uint32_t current_block_num = head_block_num();

   if( my->pending ) {
      ++current_block_num;
   }

   return my->protocol_features.is_builtin_activated( f, current_block_num );
}

bool controller::is_known_unexpired_transaction( const transaction_id_type& id) const {
   return db().find<transaction_object, by_trx_id>(id);
}

void controller::set_subjective_cpu_leeway(fc::microseconds leeway) {
   my->subjective_cpu_leeway = leeway;
}

void controller::add_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.insert(name);
}

void controller::remove_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.erase(name);
}

bool controller::is_resource_greylisted(const account_name &name) const {
   return my->conf.resource_greylist.find(name) !=  my->conf.resource_greylist.end();
}

const flat_set<account_name> &controller::get_resource_greylist() const {
   return  my->conf.resource_greylist;
}


void controller::add_to_ram_correction( account_name account, uint64_t ram_bytes ) {
   if( auto ptr = my->db.find<account_ram_correction_object, by_name>( account ) ) {
      my->db.modify<account_ram_correction_object>( *ptr, [&]( auto& rco ) {
         rco.ram_correction += ram_bytes;
      } );
   } else {
      my->db.create<account_ram_correction_object>( [&]( auto& rco ) {
         rco.name = account;
         rco.ram_correction = ram_bytes;
      } );
   }
}

bool controller::all_subjective_mitigations_disabled()const {
   return my->conf.disable_all_subjective_mitigations;
}

fc::optional<uint64_t> controller::convert_exception_to_error_code( const fc::exception& e ) {
   const chain_exception* e_ptr = dynamic_cast<const chain_exception*>( &e );

   if( e_ptr == nullptr ) return {};

   if( !e_ptr->error_code ) return static_cast<uint64_t>(system_error_code::generic_system_error);

   return e_ptr->error_code;
}

/// Protocol feature activation handlers:

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::preactivate_feature>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "preactivate_feature" );
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "is_feature_activated" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::get_sender>() {
   db.modify( db.get<protocol_state_object>(), [&]( auto& ps ) {
      add_intrinsic_to_whitelist( ps.whitelisted_intrinsics, "get_sender" );
   } );
}

template<>
void controller_impl::on_activation<builtin_protocol_feature_t::replace_deferred>() {
   const auto& indx = db.get_index<account_ram_correction_index, by_id>();
   for( auto itr = indx.begin(); itr != indx.end(); itr = indx.begin() ) {
      int64_t current_ram_usage = resource_limits.get_account_ram_usage( itr->name );
      int64_t ram_delta = -static_cast<int64_t>(itr->ram_correction);
      if( itr->ram_correction > static_cast<uint64_t>(current_ram_usage) ) {
         ram_delta = -current_ram_usage;
         elog( "account ${name} was to be reduced by ${adjust} bytes of RAM despite only using ${current} bytes of RAM",
               ("name", itr->name)("adjust", itr->ram_correction)("current", current_ram_usage) );
      }

      resource_limits.add_pending_ram_usage( itr->name, ram_delta );
      db.remove( *itr );
   }
}

/// End of protocol feature activation handlers

} } /// eosio::chain
