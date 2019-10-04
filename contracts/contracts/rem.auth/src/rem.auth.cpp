/**
 *  @copyright defined in eos/LICENSE.txt
 */

#include <rem.auth/rem.auth.hpp>
#include <../../rem.swap/src/base58.cpp> // TODO: fix includes
#include <rem.system/rem.system.hpp>
#include <rem.token/rem.token.hpp>

namespace eosio {

   using eosiosystem::system_contract;

   void auth::buyauth(const name &account, const asset &quantity, const double &max_price) {
      require_auth(account);
      check(quantity.is_valid(), "invalid quantity");
      check(quantity.amount > 0, "quantity should be a positive value");
      check(quantity.symbol == auth_symbol, "symbol precision mismatch");

//      double remusd = get_remusd_price(); // TODO: implement method get_remusd_price from oracle table remusd
      double remusd = 0.002819;

      check(max_price > remusd, "currently rem-usd price is above maximum price");
      asset purchase_fee = get_authrem_price(quantity);

      transfer_tokens(account, _self, purchase_fee, "purchase fee AUTH tokens");
      issue_tokens(quantity);
      transfer_tokens(_self, account, quantity, "buying an auth token");
   }

   void auth::addkeyacc(const name &account, const string &key_str, const signature &signed_by_key,
                        const string &extra_key, const asset &price_limit, const string &payer_str) {

      name payer = payer_str.empty() ? account : name(payer_str);
      require_auth(account);
      require_auth(payer);

      public_key key = string_to_public_key(key_str);
      checksum256 digest = sha256(join( { account.to_string(), key_str, extra_key, payer_str } ));
      eosio::assert_recover_key(digest, signed_by_key, key);

      authkeys_tbl.emplace(_self, [&](auto &k) {
         k.N = authkeys_tbl.available_primary_key();
         k.owner = account;
         k.key = key;
         k.extra_key = extra_key;
         k.not_valid_before = current_time_point();
         k.not_valid_after = current_time_point() + key_lifetime;
         k.revoked_at = 0; // if not revoked == 0
      });

      sub_storage_fee(payer, price_limit);
   }

   auto auth::get_authkey_it(const name &account, const public_key &key) {
      auto authkeys_idx = authkeys_tbl.get_index<"byname"_n>();
      auto it = authkeys_idx.find(account.value);

      for(; it != authkeys_idx.end(); ++it) {
         auto ct = current_time_point();

         bool is_before_time_valid = ct > it->not_valid_before.to_time_point();
         bool is_after_time_valid = ct < it->not_valid_after.to_time_point();
         bool is_revoked = it->revoked_at;

         if (!is_before_time_valid || !is_after_time_valid || is_revoked) {
            continue;
         } else if (it->key == key) {
            break;
         }
      }
      return it;
   }

   void auth::addkeyapp(const name &account, const string &new_key_str, const signature &signed_by_new_key,
                        const string &extra_key, const string &key_str, const signature &signed_by_key,
                        const asset &price_limit, const string &payer_str) {

      bool is_payer = payer_str.empty();
      name payer = is_payer ? account : name(payer_str);
      if (!is_payer) { require_auth(payer); }

      checksum256 digest = sha256(join( { account.to_string(), new_key_str, extra_key, key_str, payer_str } ));

      public_key new_key = string_to_public_key(new_key_str);
      public_key key = string_to_public_key(key_str);

      check(assert_recover_key(digest, signed_by_new_key, new_key), "expected key different than recovered new key");
      check(assert_recover_key(digest, signed_by_key, key), "expected key different than recovered account key");
      require_app_auth(account, key);

      authkeys_tbl.emplace(_self, [&](auto &k) {
         k.N = authkeys_tbl.available_primary_key();
         k.owner = account;
         k.key = key;
         k.extra_key = extra_key;
         k.not_valid_before = current_time_point();
         k.not_valid_after = current_time_point() + key_lifetime;
         k.revoked_at = 0; // if not revoked == 0
      });

      sub_storage_fee(payer, price_limit);
   }

   void auth::revokeacc(const name &account, const string &key_str) {
      require_auth(account);
      public_key key = string_to_public_key(key_str);

      revoke_key(account, key);
   }

   void auth::revokeapp(const name &account, const string &revocation_key_str,
                        const string &key_str, const signature &signed_by_key) {
      public_key revocation_key = string_to_public_key(revocation_key_str);
      public_key key = string_to_public_key(key_str);

      checksum256 digest = sha256(join( { account.to_string(), revocation_key_str, key_str } ));

      public_key expected_key = recover_key(digest, signed_by_key);
      check(expected_key == key, "expected key different than recovered account key");
      require_app_auth(account, key);

      revoke_key(account, revocation_key);
   }

   void auth::transfer(const name &from, const name &to, const asset &quantity,
                       const string &key_str, const signature &signed_by_key) {

      checksum256 digest = sha256(join( { from.to_string(), to.to_string(), quantity.to_string(), key_str } ));

      public_key key = string_to_public_key(key_str);
      require_app_auth(from, key);
      check(assert_recover_key(digest, signed_by_key, key), "expected key different than recovered account key");

      transfer_tokens(from, to, quantity, string("authentication app transfer"));
   }

   void auth::cleartable( ) {
      require_auth(_self);
      for (auto _table_itr = authkeys_tbl.begin(); _table_itr != authkeys_tbl.end();) {
         _table_itr = authkeys_tbl.erase(_table_itr);
      }
   }

   void auth::sub_storage_fee(const name &account, const asset &price_limit) {

      bool is_pay_by_auth = price_limit.symbol == auth_symbol;
      bool is_pay_by_rem = price_limit.symbol == system_contract::get_core_symbol();
      check(is_pay_by_rem || is_pay_by_auth, "unavailable payment method");
      check(price_limit.is_valid(), "invalid price limit");
      check(price_limit.amount > 0, "price limit should be a positive value");

      asset account_auth_balance = get_balance(system_contract::token_account, account, auth_symbol);
      asset authrem_price{0, auth_symbol};
      asset buyauth_unit_price{0, auth_symbol};

      asset auth_credit_supply = token::get_supply(system_contract::token_account, auth_symbol.code());
      asset rem_balance = token::get_balance(system_contract::token_account, _self,
                                             system_contract::get_core_symbol().code());

      if (is_pay_by_rem) {
         authrem_price = get_authrem_price(key_store_price);
         check(authrem_price < price_limit, "currently rem-usd price is above price limit");
         buyauth_unit_price = key_store_price;
         transfer_tokens(account, _self, authrem_price, "purchase fee REM tokens");
      } else {
         check(auth_credit_supply.amount > 0, "overdrawn balance");
         transfer_tokens(account, _self, key_store_price, "purchase fee AUTH tokens");
         retire_tokens(key_store_price);
      }

      double reward_amount = (rem_balance.amount + authrem_price.amount) /
         double(auth_credit_supply.amount + buyauth_unit_price.amount);

      to_rewards(_self, asset{static_cast<int64_t>(reward_amount * 10000), system_contract::get_core_symbol()});
   }

   void auth::revoke_key(const name &account, const public_key &key) {
      require_app_auth(account, key);

      auto it = get_authkey_it(account, key);

      time_point ct = current_time_point();
      authkeys_tbl.modify(*it, _self, [&](auto &r) {
         r.revoked_at = ct.sec_since_epoch();
      });
   }

   void auth::require_app_auth(const name &account, const public_key &key) {
      auto authkeys_idx = authkeys_tbl.get_index<"byname"_n>();
      auto it = authkeys_idx.find(account.value);

      check(it != authkeys_idx.end(), "account has no linked app keys");

      it = get_authkey_it(account, key);
      check(it != authkeys_idx.end(), "account has no active app keys");
   }

   asset auth::get_balance(const name& token_contract_account, const name& owner, const symbol& sym) {
      accounts accountstable(token_contract_account, owner.value);
      const auto it = accountstable.find(sym.code().raw());
      return it == accountstable.end() ? asset{0, sym} : it->balance;
   }

   asset auth::get_authrem_price(const asset &quantity) {
//      double remusd = get_remusd_price();
      double remusd = 0.002819;
      double rem_amount = quantity.amount / remusd;
      return asset{static_cast<int64_t>(rem_amount), system_contract::get_core_symbol()};
   }

   void auth::issue_tokens(const asset &quantity) {
      token::issue_action issue(system_contract::token_account, {_self, system_contract::active_permission});
      issue.send(_self, quantity, string("buy auth tokens"));
   }

   void auth::retire_tokens(const asset &quantity) {
      token::retire_action retire(system_contract::token_account, {_self, system_contract::active_permission});
      retire.send(quantity, string("store auth key"));
   }

   void auth::transfer_tokens(const name &from, const name &to, const asset &quantity, const string &memo) {
      token::transfer_action transfer(system_contract::token_account, {from, system_contract::active_permission});
      transfer.send(from, to, quantity, memo);
   }

   void auth::to_rewards(const name &payer, const asset &quantity) {
      system_contract::torewards_action torewards(system_account, {payer, system_contract::active_permission});
      torewards.send(payer, quantity);
   }

   bool auth::assert_recover_key(const checksum256 &digest, const signature &sign, const public_key &key) {
      public_key expected_key = recover_key(digest, sign);
      return expected_key == key;
   }

   string auth::join( vector<string>&& vec, string delim ) {
      return std::accumulate(std::next(vec.begin()), vec.end(), vec[0],
                             [&delim](string& a, string& b) {
                                return a + delim + b;
                             });
   }
} /// namespace eosio

EOSIO_DISPATCH( eosio::auth, (addkeyacc)(addkeyapp)(revokeacc)(revokeapp)(buyauth)(transfer)(cleartable) )
