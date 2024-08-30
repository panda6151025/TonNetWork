#include "td/actor/actor.h"

#include "td/utils/filesystem.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/overloaded.h"
#include "td/utils/Parser.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"
#include "td/utils/as.h"
//#include "td/utils/SharedSlice.h"

#include "terminal/terminal.h"

#include "tonlib/TonlibClient.h"
#include "tonlib/TonlibCallback.h"

#include "tonlib/ExtClientLazy.h"

#include "auto/tl/tonlib_api.hpp"

#include "vm/cells/Cell.h"

#include <cinttypes>
#include <iostream>
#include <map>

#include <staking/staking-smc-envelope/GenericAccount.h>
#include <staking/staking-smc-envelope/WalletV3.h>
// #include <staking/staking-smc-envelope/MultisigWallet.h>
#include "staking/staking-smc-envelope/StakingPool.h"
#include "staking/staking-smc-envelope/Nominator.h"


struct Grams {
    td::uint64 nano;
};


td::StringBuilder& operator<<(td::StringBuilder& sb, const Grams& grams) {
    auto b = grams.nano % 1000000000;
    auto a = grams.nano / 1000000000;
    sb << "GR$" << a;
    if (b != 0) {
        size_t sz = 9;
        while (b % 10 == 0) {
            sz--;
            b /= 10;
        }
        sb << '.';
        [&](auto b_str) {
            for (size_t i = b_str.size(); i < sz; i++) {
                sb << '0';
            }
            sb << b_str;
        }(PSLICE() << b);
    }
    return sb;
}


td::Result<Grams> parse_grams(td::Slice grams) {
    td::ConstParser parser(grams);
    if (parser.skip_start_with("GR$")) {
        TRY_RESULT(a, td::to_integer_safe<td::uint32>(parser.read_till_nofail('.')));
        td::uint64 res = a;
        if (parser.try_skip('.')) {
            for (int i = 0; i < 9; i++) {
                res *= 10;
                if (parser.peek_char() >= '0' && parser.peek_char() <= '9') {
                    res += parser.peek_char() - '0';
                    parser.advance(1);
                }
            }
        } else {
            res *= 1000000000;
        }
        if (!parser.empty()) {
            return td::Status::Error(PSLICE() << "Failed to parse grams \"" << grams << "\", left \"" << parser.read_all()
                                              << "\"");
        }
        return Grams{res};
    }
    TRY_RESULT(value, td::to_integer_safe<td::uint64>(grams));
    return Grams{value};
}

class TonlibCli : public td::actor::Actor {
 public:
    struct Options {
        bool enable_readline{true};
        std::string config;
        std::string name;
        std::string key_dir{"."};
        bool in_memory{false};
        bool use_callbacks_for_network{false};
        td::int32 wallet_version = 2;
        td::int32 wallet_revision = 0;
        td::optional<td::uint32> wallet_id;
        bool ignore_cache{false};

        bool one_shot{false};
        std::string cmd;
    };
  TonlibCli(Options options) : options_(std::move(options)) {
  }

 private:
  Options options_;
  td::actor::ActorOwn<td::TerminalIO> io_;
  td::actor::ActorOwn<tonlib::TonlibClient> client_;
  std::uint64_t next_query_id_{1};
  td::Promise<td::Slice> cont_;
  td::uint32 wallet_id_;


  struct KeyInfo {
    std::string public_key;
    td::SecureString secret;
  };
  std::vector<KeyInfo> keys_;

  std::map<std::uint64_t, td::Promise<tonlib_api::object_ptr<tonlib_api::Object>>> query_handlers_;

  td::actor::ActorOwn<ton::adnl::AdnlExtClient> raw_client_;

  bool is_closing_{false};
  td::uint32 ref_cnt_{1};

  td::int64 snd_bytes_{0};
  td::int64 rcv_bytes_{0};

  void start_up() override {
    class Cb : public td::TerminalIO::Callback {
     public:
      void line_cb(td::BufferSlice line) override {
        td::actor::send_closure(id_, &TonlibCli::parse_line, std::move(line));
      }
      Cb(td::actor::ActorShared<TonlibCli> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorShared<TonlibCli> id_;
    };
    ref_cnt_++;
    if (!options_.one_shot) {
      io_ = td::TerminalIO::create("> ", options_.enable_readline, std::make_unique<Cb>(actor_shared(this)));
      td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);
    }

    class TonlibCb : public tonlib::TonlibCallback {
     public:
      TonlibCb(td::actor::ActorShared<TonlibCli> id) : id_(std::move(id)) {
      }
      void on_result(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::Object> result) override {
        send_closure(id_, &TonlibCli::on_tonlib_result, id, std::move(result));
      }
      void on_error(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::error> error) override {
        send_closure(id_, &TonlibCli::on_tonlib_error, id, std::move(error));
      }

     private:
      td::actor::ActorShared<TonlibCli> id_;
    };
    ref_cnt_++;
    client_ = td::actor::create_actor<tonlib::TonlibClient>("Tonlib", td::make_unique<TonlibCb>(actor_shared(this, 1)));

    td::mkdir(options_.key_dir).ignore();

    load_keys();

    if (options_.use_callbacks_for_network) {
      auto config = tonlib::Config::parse(options_.config).move_as_ok();
      auto lite_clients_size = config.lite_clients.size();
      CHECK(lite_clients_size != 0);
      auto lite_client_id = td::Random::fast(0, td::narrow_cast<int>(lite_clients_size) - 1);
      auto& lite_client = config.lite_clients[lite_client_id];
      class Callback : public tonlib::ExtClientLazy::Callback {
       public:
        explicit Callback(td::actor::ActorShared<> parent) : parent_(std::move(parent)) {
        }

       private:
        td::actor::ActorShared<> parent_;
      };
      ref_cnt_++;
      raw_client_ = tonlib::ExtClientLazy::create(lite_client.adnl_id, lite_client.address,
                                                  td::make_unique<Callback>(td::actor::actor_shared()));
    }

    using tonlib_api::make_object;
    auto config = !options_.config.empty()
                      ? make_object<tonlib_api::config>(options_.config, options_.name,
                                                        options_.use_callbacks_for_network, options_.ignore_cache)
                      : nullptr;

    tonlib_api::object_ptr<tonlib_api::KeyStoreType> ks_type;
    if (options_.in_memory) {
      ks_type = make_object<tonlib_api::keyStoreTypeInMemory>();
    } else {
      ks_type = make_object<tonlib_api::keyStoreTypeDirectory>(options_.key_dir);
    }
    send_query(make_object<tonlib_api::init>(make_object<tonlib_api::options>(std::move(config), std::move(ks_type))),
               [&](auto r_ok) {
                 LOG_IF(ERROR, r_ok.is_error()) << r_ok.error();
                 if (r_ok.is_ok()) {
                   wallet_id_ = static_cast<td::uint32>(r_ok.ok()->config_info_->default_wallet_id_);
                   td::TerminalIO::out() << "Tonlib is inited\n";
                 }
               });
    if (options_.one_shot) {
      td::actor::send_closure(actor_id(this), &TonlibCli::parse_line, td::BufferSlice(options_.cmd));
    }
  }

  void hangup_shared() override {
    CHECK(ref_cnt_ > 0);
    ref_cnt_--;
    if (get_link_token() == 1) {
      io_.reset();
    }
    try_stop();
  }
  void try_stop() {
    if (is_closing_ && ref_cnt_ == 0) {
      stop();
    }
  }
  void tear_down() override {
    td::actor::SchedulerContext::get()->stop();
  }

  void on_wait() {
    if (options_.one_shot) {
      LOG(ERROR) << "FAILED (not enough data)";
      std::_Exit(2);
    }
  }

  void on_error() {
    if (options_.one_shot) {
      LOG(ERROR) << "FAILED";
      std::_Exit(1);
    }
  }
  void on_ok() {
    if (options_.one_shot) {
      LOG(INFO) << "OK";
      std::_Exit(0);
    }
  }

  void parse_line(td::BufferSlice line) {
    if (is_closing_) {
      return;
    }
    if (cont_) {
      auto cont = std::move(cont_);
      cont.set_value(line.as_slice());
      return;
    }
    td::ConstParser parser(line.as_slice());
    auto cmd = parser.read_word();
    if (cmd.empty()) {
      return;
    }
    auto to_bool = [](td::Slice word, bool def = false) {
      if (word.empty()) {
        return def;
      }
      if (word == "0" || word == "FALSE" || word == "false") {
        return false;
      }
      return true;
    };

    td::Promise<td::Unit> cmd_promise = [line = line.clone()](td::Result<td::Unit> res) {
      if (res.is_ok()) {
        // on_ok
      } else {
        td::TerminalIO::out() << "Query {" << line.as_slice() << "} FAILED: \n\t" << res.error() << "\n";
      }
    };

    if (cmd == "help") {
      td::TerminalIO::out() << "help\tThis help\n";
      td::TerminalIO::out() << "time\tGet server time\n";
      td::TerminalIO::out() << "remote-version\tShows server time, version and capabilities\n";
      td::TerminalIO::out() << "sendfile <filename>\tLoad a serialized message from <filename> and send it to server\n";
      td::TerminalIO::out() << "setconfig|validateconfig <path> [<name>] [<use_callback>] [<force>] - set or validate "
                               "lite server config\n";
      td::TerminalIO::out() << "exit\tExit\n";
      td::TerminalIO::out() << "quit\tExit\n";
      td::TerminalIO::out()
          << "saveaccount[code|data] <filename> <addr>\tSaves into specified file the most recent state\n";

      td::TerminalIO::out() << "genkey - generate new secret key\n";
      td::TerminalIO::out() << "keys - show all stored keys\n";
      td::TerminalIO::out() << "unpackaddress <address> - validate and parse address\n";
      td::TerminalIO::out() << "setbounceble <address> [<bounceble>] - change bounceble flag in address\n";
      td::TerminalIO::out() << "importkey - import key\n";
      td::TerminalIO::out() << "deletekeys - delete ALL PRIVATE KEYS\n";
      td::TerminalIO::out() << "exportkey [<key_id>] - export key\n";
      td::TerminalIO::out() << "exportkeypem [<key_id>] - export key\n";
      td::TerminalIO::out() << "getstate <key_id> - get state of simple wallet with requested key\n";
      td::TerminalIO::out()
          << "gethistory <key_id> - get history fo simple wallet with requested key (last 10 transactions)\n";
      td::TerminalIO::out() << "inits <key_id> - init simple wallet with requested key\n";
      td::TerminalIO::out() << "transfer[f] <from_key_id> <to_key_id> <amount> - transfer <amount> of grams from "
                               "<from_key_id> to <to_key_id>.\n"
                            << "\t<from_key_id> could also be 'giver'\n"
                            << "\t<to_key_id> could also be 'giver' or smartcontract address\n";
      td::TerminalIO::out() << "address <key_id> <smc-type> [<wallet-id>] - show the address of smart contract"
                               "for <key_id>.\n"
                            << "\t<smc-type> could be 'wallet', 'owner', 'pool', 'nominator'\n"
                            << "\t<wallet-id> could be specified for 'wallet', must be specified for 'nominator'\n"
                            << "\tand should be empty of 0 for 'owner' and 'pool'\n";
      td::TerminalIO::out() << "init <key_id> <smc-type> [<wallet-id>] - sends initialization message to specified smart contract"
                               "for <key_id>.\n"
                            << "\t<smc-type> could be 'wallet', 'owner', 'pool', 'nominator'\n"
                            << "\t<wallet-id> could be specified for 'wallet', must be specified for 'nominator'\n"
                            << "\tand should be empty of 0 for 'owner' and 'pool'\n";

    } else if (cmd == "genkey") {
      generate_key();
    } else if (cmd == "exit" || cmd == "quit") {
      is_closing_ = true;
      client_.reset();
      ref_cnt_--;
      try_stop();
    } else if (cmd == "keys") {
      dump_keys();
    } else if (cmd == "pool") {
      auto key = parser.read_word();
      auto command = parser.read_word();
      auto param1 = parser.read_word();
      auto param2 = parser.read_word();
      pool_command(key, command, param1, param2);
    } else if (cmd == "address") {
        auto key = parser.read_word();
        auto smc_type = parser.read_word();
        auto wallet_idx = parser.read_word();
        show_key(key, smc_type, wallet_idx);
    } else if (cmd == "deletekey") {
      //delete_key(parser.read_word());
    } else if (cmd == "deletekeys") {
      delete_all_keys();
    } else if (cmd == "exportkey" || cmd == "exportkeypem") {
      export_key(cmd.str(), parser.read_word());
    } else if (cmd == "importkey") {
      import_key(parser.read_all());
    } else if (cmd == "getstate") {
      get_state(parser.read_word(), std::move(cmd_promise));
    } else if (cmd == "init") {
      auto key = parser.read_word();
      auto smc_type = parser.read_word();
      auto wallet_idx = parser.read_word();
      init_account(key, smc_type, wallet_idx);
    } else if (cmd == "transfer" || cmd == "transferf") {
        transfer(parser, cmd, std::move(cmd_promise));
    } else if (cmd == "hint") {
      get_hints(parser.read_word());
    } else if (cmd == "unpackaddress") {
      unpack_address(parser.read_word());
    } else if (cmd == "setbounceable") {
      auto addr = parser.read_word();
      auto bounceable = parser.read_word();
      set_bounceable(addr, to_bool(bounceable, true));
    } else if (cmd == "netstats") {
      dump_netstats();
      // reviewed from here
    } else if (cmd == "sync") {
      sync(std::move(cmd_promise));
    } else if (cmd == "time") {
      remote_time(std::move(cmd_promise));
    } else if (cmd == "remote-version") {
      remote_version(std::move(cmd_promise));
    } else if (cmd == "sendfile") {
      send_file(parser.read_word(), std::move(cmd_promise));
    } else if (cmd == "saveaccount" || cmd == "saveaccountdata" || cmd == "saveaccountcode") {
      auto path = parser.read_word();
      auto address = parser.read_word();
      save_account(cmd, path, address, std::move(cmd_promise));
    } else if (cmd == "runmethod") {
      run_method(parser, std::move(cmd_promise));
    } else if (cmd == "setconfig" || cmd == "validateconfig") {
      auto config = parser.read_word();
      auto name = parser.read_word();
      auto use_callback = parser.read_word();
      auto force = parser.read_word();
      set_validate_config(cmd, config, name, to_bool(use_callback), to_bool(force), std::move(cmd_promise));
    } else {
      cmd_promise.set_error(td::Status::Error(PSLICE() << "Unkwnown query `" << cmd << "`"));
    }
    if (cmd_promise) {
      cmd_promise.set_value(td::Unit());
    }
  }

  void remote_time(td::Promise<td::Unit> promise) {
    send_query(tonlib_api::make_object<tonlib_api::liteServer_getInfo>(), promise.wrap([](auto&& info) {
      td::TerminalIO::out() << "Lite server time is: " << info->now_ << "\n";
      return td::Unit();
    }));
  }

  void remote_version(td::Promise<td::Unit> promise) {
    send_query(tonlib_api::make_object<tonlib_api::liteServer_getInfo>(), promise.wrap([](auto&& info) {
      td::TerminalIO::out() << "Lite server time is: " << info->now_ << "\n";
      td::TerminalIO::out() << "Lite server version is: " << info->version_ << "\n";
      td::TerminalIO::out() << "Lite server capabilities are: " << info->capabilities_ << "\n";
      return td::Unit();
    }));
  }

  void send_file(td::Slice name, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, data, td::read_file_str(name.str()));
    send_query(tonlib_api::make_object<tonlib_api::raw_sendMessage>(std::move(data)), promise.wrap([](auto&& info) {
      td::TerminalIO::out() << "Query was sent\n";
      return td::Unit();
    }));
  }

  void save_account(td::Slice cmd, td::Slice path, td::Slice address, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, addr, to_account_address(address, false));
    send_query(tonlib_api::make_object<tonlib_api::smc_load>(std::move(addr.address)),
               promise.send_closure(actor_id(this), &TonlibCli::save_account_2, cmd.str(), path.str(), address.str()));
  }

  void save_account_2(std::string cmd, std::string path, std::string address,
                      tonlib_api::object_ptr<tonlib_api::smc_info> info, td::Promise<td::Unit> promise) {
    auto with_query = [&, self = this](auto query, auto log) {
      send_query(std::move(query),
                 promise.send_closure(actor_id(self), &TonlibCli::save_account_3, std::move(path), std::move(log)));
    };
    if (cmd == "saveaccount") {
      with_query(tonlib_api::make_object<tonlib_api::smc_getState>(info->id_),
                 PSTRING() << "StateInit of account " << address);
    } else if (cmd == "saveaccountcode") {
      with_query(tonlib_api::make_object<tonlib_api::smc_getCode>(info->id_), PSTRING()
                                                                                  << "Code of account " << address);
    } else if (cmd == "saveaccountdata") {
      with_query(tonlib_api::make_object<tonlib_api::smc_getData>(info->id_), PSTRING()
                                                                                  << "Data of account " << address);
    } else {
      promise.set_error(td::Status::Error("Unknown query"));
    }
  }

  void save_account_3(std::string path, std::string log, tonlib_api::object_ptr<tonlib_api::tvm_cell> cell,
                      td::Promise<td::Unit> promise) {
    TRY_STATUS_PROMISE(promise, td::write_file(path, cell->bytes_));
    td::TerminalIO::out() << log << " was successfully written to the disk(" << td::format::as_size(cell->bytes_.size())
                          << ")\n";
    promise.set_value(td::Unit());
  }

  void sync(td::Promise<td::Unit> promise) {
    using tonlib_api::make_object;
    send_query(make_object<tonlib_api::sync>(), promise.wrap([](auto&&) {
      td::TerminalIO::out() << "synchronized\n";
      return td::Unit();
    }));
  }
  td::Result<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>> parse_stack_entry(td::Slice str) {
    if (str.empty() || str.size() > 65535) {
      return td::Status::Error("String is or empty or too big");
    }
    int l = (int)str.size();
    if (str[0] == '"') {
      vm::CellBuilder cb;
      if (l == 1 || str.back() != '"' || l >= 127 + 2 || !cb.store_bytes_bool(str.data() + 1, l - 2)) {
        return td::Status::Error("Failed to parse slice");
      }
      return tonlib_api::make_object<tonlib_api::tvm_stackEntrySlice>(
          tonlib_api::make_object<tonlib_api::tvm_slice>(vm::std_boc_serialize(cb.finalize()).ok().as_slice().str()));
    }
    if (l >= 3 && (str[0] == 'x' || str[0] == 'b') && str[1] == '{' && str.back() == '}') {
      unsigned char buff[128];
      int bits =
          (str[0] == 'x')
              ? (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), str.begin() + 2, str.end() - 1)
              : (int)td::bitstring::parse_bitstring_binary_literal(buff, sizeof(buff), str.begin() + 2, str.end() - 1);
      if (bits < 0) {
        return td::Status::Error("Failed to parse slice");
      }
      return tonlib_api::make_object<tonlib_api::tvm_stackEntrySlice>(tonlib_api::make_object<tonlib_api::tvm_slice>(
          vm::std_boc_serialize(vm::CellBuilder().store_bits(td::ConstBitPtr{buff}, bits).finalize())
              .ok()
              .as_slice()
              .str()));
    }
    auto num = td::RefInt256{true};
    auto& x = num.unique_write();
    if (l >= 3 && str[0] == '0' && str[1] == 'x') {
      if (x.parse_hex(str.data() + 2, l - 2) != l - 2) {
        return td::Status::Error("Failed to parse a number");
      }
    } else if (l >= 4 && str[0] == '-' && str[1] == '0' && str[2] == 'x') {
      if (x.parse_hex(str.data() + 3, l - 3) != l - 3) {
        return td::Status::Error("Failed to parse a number");
      }
      x.negate().normalize();
    } else if (!l || x.parse_dec(str.data(), l) != l) {
      return td::Status::Error("Failed to parse a number");
    }
    return tonlib_api::make_object<tonlib_api::tvm_stackEntryNumber>(
        tonlib_api::make_object<tonlib_api::tvm_numberDecimal>(dec_string(num)));
  }

  td::Result<std::vector<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>>> parse_stack(td::ConstParser& parser,
                                                                                          td::Slice end_token) {
    std::vector<tonlib_api::object_ptr<tonlib_api::tvm_StackEntry>> stack;
    while (true) {
      auto word = parser.read_word();
      LOG(ERROR) << word << " vs " << end_token;
      if (word == end_token) {
        break;
      }
      if (word == "[") {
        TRY_RESULT(elements, parse_stack(parser, "]"));
        stack.push_back(tonlib_api::make_object<tonlib_api::tvm_stackEntryTuple>(
            tonlib_api::make_object<tonlib_api::tvm_tuple>(std::move(elements))));
      } else if (word == "(") {
        TRY_RESULT(elements, parse_stack(parser, ")"));
        stack.push_back(tonlib_api::make_object<tonlib_api::tvm_stackEntryList>(
            tonlib_api::make_object<tonlib_api::tvm_list>(std::move(elements))));
      } else {
        TRY_RESULT(stack_entry, parse_stack_entry(word));
        stack.push_back(std::move(stack_entry));
      }
    }
    return std::move(stack);
  }

  static void store_entry(td::StringBuilder& sb, tonlib_api::tvm_StackEntry& entry) {
    downcast_call(entry, td::overloaded(
                             [&](tonlib_api::tvm_stackEntryCell& cell) {
                               auto r_cell = vm::std_boc_deserialize(cell.cell_->bytes_);
                               if (r_cell.is_error()) {
                                 sb << "<INVALID_CELL>";
                               }
                               auto cs = vm::load_cell_slice(r_cell.move_as_ok());
                               std::stringstream ss;
                               cs.print_rec(ss);
                               sb << ss.str();
                             },
                             [&](tonlib_api::tvm_stackEntrySlice& cell) {
                               auto r_cell = vm::std_boc_deserialize(cell.slice_->bytes_);
                               if (r_cell.is_error()) {
                                 sb << "<INVALID_CELL>";
                               }
                               auto cs = vm::load_cell_slice(r_cell.move_as_ok());
                               std::stringstream ss;
                               cs.print_rec(ss);
                               sb << ss.str();
                             },
                             [&](tonlib_api::tvm_stackEntryNumber& cell) { sb << cell.number_->number_; },
                             [&](tonlib_api::tvm_stackEntryTuple& cell) {
                               sb << "(";
                               for (auto& element : cell.tuple_->elements_) {
                                 sb << " ";
                                 store_entry(sb, *element);
                               }
                               sb << " )";
                             },
                             [&](tonlib_api::tvm_stackEntryList& cell) {
                               sb << "[";
                               for (auto& element : cell.list_->elements_) {
                                 sb << " ";
                                 store_entry(sb, *element);
                               }
                               sb << " ]";
                             },
                             [&](tonlib_api::tvm_stackEntryUnsupported& cell) { sb << "<UNSUPPORTED>"; }));
  }

  void run_method(td::ConstParser& parser, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, addr, to_account_address(parser.read_word(), false));

    auto method_str = parser.read_word();
    tonlib_api::object_ptr<tonlib_api::smc_MethodId> method;
    if (std::all_of(method_str.begin(), method_str.end(), [](auto c) { return c >= '0' && c <= '9'; })) {
      method = tonlib_api::make_object<tonlib_api::smc_methodIdNumber>(td::to_integer<td::int32>(method_str.str()));
    } else {
      method = tonlib_api::make_object<tonlib_api::smc_methodIdName>(method_str.str());
    }
    TRY_RESULT_PROMISE(promise, stack, parse_stack(parser, ""));
    td::StringBuilder sb;
    for (auto& entry : stack) {
      store_entry(sb, *entry);
      sb << "\n";
    }

    td::TerminalIO::out() << "Run " << to_string(method) << "With stack:\n" << sb.as_cslice();

    auto to_run =
        tonlib_api::make_object<tonlib_api::smc_runGetMethod>(0 /*fixme*/, std::move(method), std::move(stack));

    send_query(tonlib_api::make_object<tonlib_api::smc_load>(std::move(addr.address)),
               promise.send_closure(actor_id(this), &TonlibCli::run_method_2, std::move(to_run)));
  }

  void run_method_2(tonlib_api::object_ptr<tonlib_api::smc_runGetMethod> to_run,
                    tonlib_api::object_ptr<tonlib_api::smc_info> info, td::Promise<td::Unit> promise) {
    to_run->id_ = info->id_;
    send_query(std::move(to_run), promise.send_closure(actor_id(this), &TonlibCli::run_method_3));
  }
  void run_method_3(tonlib_api::object_ptr<tonlib_api::smc_runResult> info, td::Promise<td::Unit> promise) {
    td::StringBuilder sb;
    for (auto& entry : info->stack_) {
      store_entry(sb, *entry);
      sb << "\n";
    }

    td::TerminalIO::out() << "Got smc result. exit code: " << info->exit_code_ << ", gas_used: " << info->gas_used_
                          << "\n"
                          << sb.as_cslice();
    promise.set_value({});
  }

  void set_validate_config(td::Slice cmd, td::Slice path, td::Slice name, bool use_callback, bool ignore_cache,
                           td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, data, td::read_file_str(path.str()));
    using tonlib_api::make_object;

    auto config = make_object<tonlib_api::config>(std::move(data), name.str(), use_callback, ignore_cache);
    if (cmd == "setconfig") {
      send_query(make_object<tonlib_api::options_setConfig>(std::move(config)), promise.wrap([](auto&& info) {
        td::TerminalIO::out() << "Config is set\n";
        return td::Unit();
      }));
    } else {
      send_query(make_object<tonlib_api::options_validateConfig>(std::move(config)), promise.wrap([](auto&& info) {
        td::TerminalIO::out() << "Config is valid: " << to_string(info) << "\n";
        return td::Unit();
      }));
    }
  }

  void dump_netstats() {
    td::TerminalIO::out() << td::tag("snd", td::format::as_size(snd_bytes_)) << "\n";
    td::TerminalIO::out() << td::tag("rcv", td::format::as_size(rcv_bytes_)) << "\n";
  }
  void on_adnl_result(td::uint64 id, td::Result<td::BufferSlice> res) {
    using tonlib_api::make_object;
    if (res.is_ok()) {
      rcv_bytes_ += res.ok().size();
      send_query(make_object<tonlib_api::onLiteServerQueryResult>(id, res.move_as_ok().as_slice().str()),
                 [](auto r_ok) { LOG_IF(ERROR, r_ok.is_error()) << r_ok.error(); });
    } else {
      send_query(make_object<tonlib_api::onLiteServerQueryError>(
                     id, make_object<tonlib_api::error>(res.error().code(), res.error().message().str())),
                 [](auto r_ok) { LOG_IF(ERROR, r_ok.is_error()) << r_ok.error(); });
    }
  }

  td::Timestamp sync_started_;

  void on_tonlib_result(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::Object> result) {
    if (id == 0) {
      switch (result->get_id()) {
        case tonlib_api::updateSendLiteServerQuery::ID: {
          auto update = tonlib_api::move_object_as<tonlib_api::updateSendLiteServerQuery>(std::move(result));
          CHECK(!raw_client_.empty());
          snd_bytes_ += update->data_.size();
          send_closure(raw_client_, &ton::adnl::AdnlExtClient::send_query, "query", td::BufferSlice(update->data_),
                       td::Timestamp::in(5),
                       [actor_id = actor_id(this), id = update->id_](td::Result<td::BufferSlice> res) {
                         send_closure(actor_id, &TonlibCli::on_adnl_result, id, std::move(res));
                       });
          return;
        }
        case tonlib_api::updateSyncState::ID: {
          auto update = tonlib_api::move_object_as<tonlib_api::updateSyncState>(std::move(result));
          switch (update->sync_state_->get_id()) {
            case tonlib_api::syncStateDone::ID: {
              td::TerminalIO::out() << "synchronization: DONE in "
                                    << td::format::as_time(td::Time::now() - sync_started_.at()) << "\n";
              sync_started_ = {};
              break;
            }
            case tonlib_api::syncStateInProgress::ID: {
              if (!sync_started_) {
                sync_started_ = td::Timestamp::now();
              }
              auto progress = tonlib_api::move_object_as<tonlib_api::syncStateInProgress>(update->sync_state_);
              auto from = progress->from_seqno_;
              auto to = progress->to_seqno_;
              auto at = progress->current_seqno_;
              auto d = to - from;
              if (d <= 0) {
                td::TerminalIO::out() << "synchronization: ???\n";
              } else {
                td::TerminalIO::out() << "synchronization: " << 100 * (at - from) / d << "%\n";
              }
              break;
            }
          }
          return;
        }
      }
    }
    auto it = query_handlers_.find(id);
    if (it == query_handlers_.end()) {
      return;
    }
    auto promise = std::move(it->second);
    query_handlers_.erase(it);
    promise.set_value(std::move(result));
  }

  void on_tonlib_error(std::uint64_t id, tonlib_api::object_ptr<tonlib_api::error> error) {
    auto it = query_handlers_.find(id);
    if (it == query_handlers_.end()) {
      return;
    }
    auto promise = std::move(it->second);
    query_handlers_.erase(it);
    promise.set_error(td::Status::Error(error->code_, error->message_));
  }

  template <class QueryT>
  void send_query(tonlib_api::object_ptr<QueryT> query, td::Promise<typename QueryT::ReturnType> promise) {
    if (is_closing_) {
      return;
    }
    auto query_id = next_query_id_++;
    td::actor::send_closure(client_, &tonlib::TonlibClient::request, query_id, std::move(query));
    query_handlers_[query_id] =
        [promise = std::move(promise)](td::Result<tonlib_api::object_ptr<tonlib_api::Object>> r_obj) mutable {
          if (r_obj.is_error()) {
            return promise.set_error(r_obj.move_as_error());
          }
          promise.set_value(ton::move_tl_object_as<typename QueryT::ReturnType::element_type>(r_obj.move_as_ok()));
        };
  }

  void unpack_address(td::Slice addr) {
    send_query(tonlib_api::make_object<tonlib_api::unpackAccountAddress>(addr.str()),
               [addr = addr.str()](auto r_parsed_addr) mutable {
                 if (r_parsed_addr.is_error()) {
                   LOG(ERROR) << "Failed to parse address: " << r_parsed_addr.error();
                   return;
                 }
                 LOG(ERROR) << to_string(r_parsed_addr.ok());
               });
  }

  void set_bounceable(td::Slice addr, bool bounceable) {
    send_query(tonlib_api::make_object<tonlib_api::unpackAccountAddress>(addr.str()),
               [addr = addr.str(), bounceable, this](auto r_parsed_addr) mutable {
                 if (r_parsed_addr.is_error()) {
                   LOG(ERROR) << "Failed to parse address: " << r_parsed_addr.error();
                   return;
                 }
                 auto parsed_addr = r_parsed_addr.move_as_ok();
                 parsed_addr->bounceable_ = bounceable;
                 this->send_query(tonlib_api::make_object<tonlib_api::packAccountAddress>(std::move(parsed_addr)),
                                  [](auto r_addr) mutable {
                                    if (r_addr.is_error()) {
                                      LOG(ERROR) << "Failed to pack address";
                                      return;
                                    }
                                    td::TerminalIO::out() << r_addr.ok()->account_address_ << "\n";
                                  });
               });
  }

  void generate_key(td::SecureString entropy = {}) {
    if (entropy.size() < 20) {
      td::TerminalIO::out() << "Enter some entropy";
      cont_ = [this, entropy = std::move(entropy)](td::Slice new_entropy) {
        td::SecureString res(entropy.size() + new_entropy.size());
        res.as_mutable_slice().copy_from(entropy.as_slice());
        res.as_mutable_slice().substr(entropy.size()).copy_from(new_entropy);
        generate_key(std::move(res));
      };
      return;
    }
    td::TerminalIO::out() << "Enter password (could be empty)";
    cont_ = [this, entropy = std::move(entropy)](td::Slice password) mutable {
      generate_key(std::move(entropy), td::SecureString(password));
    };
  }

  void generate_key(td::SecureString entropy, td::SecureString password) {
    auto password_copy = password.copy();
    send_query(tonlib_api::make_object<tonlib_api::createNewKey>(
                   std::move(password_copy), td::SecureString() /*mnemonic password*/, std::move(entropy)),
               [this, password = std::move(password)](auto r_key) mutable {
                 if (r_key.is_error()) {
                   LOG(ERROR) << "Failed to create new key: " << r_key.error();
                   return;
                 }
                 auto key = r_key.move_as_ok();
                 LOG(ERROR) << to_string(key);
                 KeyInfo info;
                 info.public_key = key->public_key_;
                 info.secret = std::move(key->secret_);
                 keys_.push_back(std::move(info));
                 export_key("exportkey", key->public_key_, keys_.size() - 1, std::move(password));
                 store_keys();
               });
  }

  void store_keys() {
    td::SecureString buf(10000);
    td::StringBuilder sb(buf.as_mutable_slice());
    for (auto& info : keys_) {
      sb << info.public_key << " " << td::base64_encode(info.secret) << "\n";
    }
    LOG_IF(FATAL, sb.is_error()) << "StringBuilder overflow";
    td::atomic_write_file(key_db_path(), sb.as_cslice());
  }

  void load_keys() {
    auto r_db = td::read_file_secure(key_db_path());
    if (r_db.is_error()) {
      return;
    }
    auto db = r_db.move_as_ok();
    td::ConstParser parser(db.as_slice());
    while (true) {
      auto public_key = parser.read_word().str();
      {
        auto tmp = td::base64_decode(public_key);
        if (tmp.is_ok()) {
          public_key = td::base64url_encode(tmp.move_as_ok());
        }
      }
      auto secret_b64 = parser.read_word();
      if (secret_b64.empty()) {
        break;
      }
      auto r_secret = td::base64_decode_secure(secret_b64);
      if (r_secret.is_error()) {
        LOG(ERROR) << "Invalid secret database at " << key_db_path();
        return;
      }

      KeyInfo info;
      info.public_key = public_key;
      info.secret = r_secret.move_as_ok();
      LOG(INFO) << info.public_key;

      keys_.push_back(std::move(info));
    }
  }

  void dump_key(size_t i) {
    td::TerminalIO::out() << "  #" << i << ": Public key: " << keys_[i].public_key << " "
                          << "    Address: "
                          << to_account_address(PSLICE() << i, false).move_as_ok().address->account_address_ << "\n";
  }
  void show_key(td::Slice key, td::Slice account_type, td::Slice wallet_idx) {
    td::TerminalIO::out() << "  #" << key << " "
                          << "    Address: "
                          << to_account_address(key, false, account_type, wallet_idx).move_as_ok().address->account_address_ << "\n";
  }


  void dump_keys() {
    td::TerminalIO::out() << "Got " << keys_.size() << " keys"
                          << "\n";
    for (size_t i = 0; i < keys_.size(); i++) {
      dump_key(i);
    }
  }



  void delete_all_keys() {
    static td::Slice password = td::Slice("I have written down mnemonic words");
    td::TerminalIO::out() << "You are going to delete ALL PRIVATE KEYS. To confirm enter `" << password << "`\n";
    cont_ = [this](td::Slice entered) {
      if (password == entered) {
        this->do_delete_all_keys();
      } else {
        td::TerminalIO::out() << "Your keys left intact\n";
      }
    };
  }

  void do_delete_all_keys() {
    send_query(tonlib_api::make_object<tonlib_api::deleteAllKeys>(), [](auto r_res) {
      if (r_res.is_error()) {
        td::TerminalIO::out() << "Something went wrong: " << r_res.error() << "\n";
        return;
      }
      td::TerminalIO::out() << "All your keys have been deleted\n";
    });
  }

  std::string key_db_path() {
    return options_.key_dir + TD_DIR_SLASH + "key_db";
  }

  td::Result<size_t> to_key_i(td::Slice key) {
    if (key.empty()) {
      return td::Status::Error("Empty key id");
    }
    if (key[0] == '#') {
      TRY_RESULT(res, td::to_integer_safe<size_t>(key.substr(1)));
      if (res < keys_.size()) {
        return res;
      }
      return td::Status::Error("Invalid key id");
    }
    auto r_res = td::to_integer_safe<size_t>(key);
    if (r_res.is_ok() && r_res.ok() < keys_.size()) {
      return r_res.ok();
    }
    if (key.size() < 3) {
      return td::Status::Error("Too short key id");
    }

    auto prefix = td::to_lower(key);
    size_t res = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < keys_.size(); i++) {
      auto full_key = td::to_lower(keys_[i].public_key);
      if (td::begins_with(full_key, prefix)) {
        res = i;
        cnt++;
      }
    }
    if (cnt == 0) {
      return td::Status::Error("Unknown key prefix");
    }
    if (cnt > 1) {
      return td::Status::Error("Non unique key prefix");
    }
    return res;
  }

  struct Address {
    tonlib_api::object_ptr<tonlib_api::accountAddress> address;
    std::string public_key;
    td::SecureString secret;
  };

    template <class F>
    auto with_account_state(int version, std::string public_key, td::uint32 wallet_id, F&& f) {
        using tonlib_api::make_object;
        if (version == 1) {
            return f(make_object<tonlib_api::testWallet_initialAccountState>(public_key));
        }
        if (version == 2) {
            return f(make_object<tonlib_api::wallet_initialAccountState>(public_key));
        }
        if (version == 4) {
            return f(make_object<tonlib_api::wallet_highload_v1_initialAccountState>(public_key, wallet_id));
        }
        if (version == 5) {
            return f(make_object<tonlib_api::wallet_highload_v2_initialAccountState>(public_key, wallet_id));
        }
        if (version == 6) {
            return f(make_object<tonlib_api::dns_initialAccountState>(public_key, wallet_id));
        }
        return f(make_object<tonlib_api::wallet_v3_initialAccountState>(public_key, wallet_id));
    }

    td::Result<Address> to_account_address(td::Slice key, bool need_private_key) {
        if (key.empty()) {
            return td::Status::Error("account address is empty");
        }
        if (key == "none" && !need_private_key) {
            return Address{};
        }
        auto r_key_i = to_key_i(key);
        using tonlib_api::make_object;
        if (r_key_i.is_ok()) {
            auto obj = [&](td::int32 version, td::int32 revision) {
                auto do_request = [revision](auto x) {
                    return tonlib::TonlibClient::static_request(
                            make_object<tonlib_api::getAccountAddress>(std::move(x), revision));
                };
                return with_account_state(version, keys_[r_key_i.ok()].public_key, wallet_id_, do_request);
            }(options_.wallet_version, options_.wallet_revision);
            if (obj->get_id() != tonlib_api::error::ID) {
                Address res;
                res.address = ton::move_tl_object_as<tonlib_api::accountAddress>(obj);
                res.public_key = keys_[r_key_i.ok()].public_key;
                res.secret = keys_[r_key_i.ok()].secret.copy();
                return std::move(res);
            }
        }
        if (key == "giver") {
            auto obj = tonlib::TonlibClient::static_request(
                    make_object<tonlib_api::getAccountAddress>(make_object<tonlib_api::testGiver_initialAccountState>(), 0));
            if (obj->get_id() != tonlib_api::error::ID) {
                Address res;
                res.address = ton::move_tl_object_as<tonlib_api::accountAddress>(obj);
                return std::move(res);
            } else {
                LOG(ERROR) << "Unexpected error during testGiver_getAccountAddress: " << to_string(obj);
            }
        }
        if (need_private_key) {
            return td::Status::Error("Don't have a private key for this address");
        }
        //TODO: validate address
        Address res;
        res.address = make_object<tonlib_api::accountAddress>(key.str());
        return std::move(res);
    }


    td::Result<Address> to_account_address(td::Slice key, bool need_private_key, td::Slice object_type, td::Slice wallet_idx) {
        if (key.empty()) {
            return td::Status::Error("account address is empty");
        }

        auto wallet_id = td::to_integer_safe<td::uint32>(wallet_idx);

        td::uint32 wallet_index=0;

        if( !wallet_id.is_error() ){
          wallet_index = wallet_id.move_as_ok();
        }

        auto r_key_i = to_key_i(key);
          using tonlib_api::make_object;
          if (r_key_i.is_ok()) {
              auto obj = [&](td::Slice version) {

                  if( version == "owner") {
                    if (wallet_index != 0) {
                      return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect Wallet ID for nominator. Must be unspecified or zero."));
                    }

                    auto pubkey = block::PublicKey::parse(keys_[r_key_i.ok()].public_key).move_as_ok();
                    auto code = vm::std_boc_serialize(ton::WalletV3::get_init_code()).move_as_ok();
                    auto code64 = code.as_slice().str();
                    auto data = vm::std_boc_serialize(ton::WalletV3::get_init_data(
                        td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                        0)).move_as_ok();
                    auto data64 = data.as_slice().str();

                    auto state = ton::WalletV3::get_init_state(
                        td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                        0);
                    auto wlt = ton::WalletV3(
                        ton::StakingSmartContract::State{ton::WalletV3::get_init_code(), ton::WalletV3::get_init_data(
                            td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                            0)});

                    auto raw_address = wlt.get_address(0);
                    auto address = raw_address.rserialize();

                    return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                        address
                    ));
                  }
                  if( version == "pool") {
                      auto pubkey = block::PublicKey::parse(keys_[r_key_i.ok()].public_key).move_as_ok();

                      auto owner_state = ton::WalletV3::get_init_state(
                          td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                          0);

                      auto owner_wallet = ton::WalletV3(
                          ton::StakingSmartContract::State{ton::WalletV3::get_init_code(),
                                ton::WalletV3::get_init_data(td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                              0)});

                      auto owner_raw_address = owner_wallet.get_address(0);

                      auto pool = ton::StakingPool(
                          ton::StakingSmartContract::State{
                              ton::StakingPool::get_init_code(),
                              ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_raw_address ),
                          });

                      auto pool_raw_address = pool.get_address(0);

                      auto address = pool_raw_address.rserialize();

                      return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                          address
                      ));
                  }
                  if( version == "nominator") {
                    if (wallet_index == 0) {
                      return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect Wallet ID for nominator. Must be specified and greater than zero."));
                    }

                    auto pubkey = block::PublicKey::parse(keys_[r_key_i.ok()].public_key).move_as_ok();

                    auto owner_state = ton::WalletV3::get_init_state(
                        td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                        0);

                    auto owner_wallet = ton::WalletV3(
                        ton::StakingSmartContract::State{ton::WalletV3::get_init_code(),
                                                         ton::WalletV3::get_init_data(td::Ed25519::PublicKey(td::SecureString(pubkey.key)),
                                                                                      0)});

                    auto owner_raw_address = owner_wallet.get_address(0);

                    auto pool = ton::StakingPool(
                        ton::StakingSmartContract::State{
                            ton::StakingPool::get_init_code(),
                            ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_raw_address),
                        });

                    auto pool_raw_address = pool.get_address(0);

                    auto nominator = ton::Nominator(
                        ton::Nominator::State{
                          ton::Nominator::get_init_code(),
                          ton::Nominator::get_init_state(pool_raw_address, wallet_index)
                        });

                    auto nominator_raw_address = nominator.get_address(-1);

                    auto address = nominator_raw_address.rserialize();

                    return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                        address
                    ));
                  }
                  //UNREACHABLE();
                  return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect SMC type use wallet/owner/pool/nominator"));
              }(object_type);
              if (obj->get_id() != tonlib_api::error::ID) {
                  Address res;
                  res.address = ton::move_tl_object_as<tonlib_api::accountAddress>(obj);
                  res.public_key = keys_[r_key_i.ok()].public_key;
                  res.secret = keys_[r_key_i.ok()].secret.copy();
                  return std::move(res);
              } else {
                LOG(ERROR) << "Error during getting an address: " << to_string(obj);
              }
          }
          if (need_private_key) {
              return td::Status::Error("Don't have a private key for this address");
          }
          //TODO: validate address
          Address res;
          res.address = make_object<tonlib_api::accountAddress>(key.str());
          return std::move(res);
    }


    td::Result<Address> to_account_address(td::Ed25519::PublicKey &public_key, td::Slice object_type, td::Slice wallet_idx) {

      auto wallet_id = td::to_integer_safe<td::uint32>(wallet_idx);

      td::uint32 wallet_index=0;

      if( !wallet_id.is_error() ){
        wallet_index = wallet_id.move_as_ok();
      }

      using tonlib_api::make_object;
        auto obj = [&](td::Slice version) {
            if( version == "wallet") {
              if(wallet_index == 0){
                wallet_index = wallet_id_;
              }

              auto code = vm::std_boc_serialize(ton::WalletV3::get_init_code()).move_as_ok();
              auto code64 = code.as_slice().str();
              auto data = vm::std_boc_serialize(ton::WalletV3::get_init_data(
                  public_key,wallet_index)).move_as_ok();
              auto data64 = data.as_slice().str();

              auto state = ton::WalletV3::get_init_state(
                  public_key,
                  wallet_index);
              auto wlt = ton::WalletV3(
                  ton::StakingSmartContract::State{ton::WalletV3::get_init_code(), ton::WalletV3::get_init_data(
                      public_key,
                      wallet_index)});

              auto raw_address = wlt.get_address(0);
              auto address = raw_address.rserialize();

                return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                        address
                ));
            }
            if( version == "owner") {
              if (wallet_index != 0) {
                return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect Wallet ID for nominator. Must be unspecified or zero."));
              }

              auto state = ton::WalletV3::get_init_state(
                  public_key,
                  0);
              auto wlt = ton::WalletV3(
                  ton::StakingSmartContract::State{ton::WalletV3::get_init_code(), ton::WalletV3::get_init_data(
                      public_key,0)});

              auto raw_address = wlt.get_address(0);
              auto address = raw_address.rserialize();

              return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                  address
              ));
            }
            if( version == "pool") {
              auto owner_state = ton::WalletV3::get_init_state(
                  public_key,
                  0);

              auto owner_wallet = ton::WalletV3(
                  ton::StakingSmartContract::State{ton::WalletV3::get_init_code(),
                                                   ton::WalletV3::get_init_data(public_key,0)});

              auto owner_raw_address = owner_wallet.get_address(0);

              auto pool = ton::StakingPool(
                  ton::StakingSmartContract::State{
                      ton::StakingPool::get_init_code(),
                      ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_raw_address),
                  });

              auto pool_raw_address = pool.get_address(0);

              auto address = pool_raw_address.rserialize();

              return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                  address
              ));
            }
            if( version == "nominator") {
              if (wallet_index == 0) {
                return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect Wallet ID for nominator. Must be specified and greater than zero."));
              }

              auto owner_state = ton::WalletV3::get_init_state(
                  public_key,
                  0);

              auto owner_wallet = ton::WalletV3(
                  ton::StakingSmartContract::State{ton::WalletV3::get_init_code(),
                                                   ton::WalletV3::get_init_data(public_key,0)});

              auto owner_raw_address = owner_wallet.get_address(0);

              auto pool = ton::StakingPool(
                  ton::StakingSmartContract::State{
                      ton::StakingPool::get_init_code(),
                      ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_raw_address),
                  });

              auto pool_raw_address = pool.get_address(0);

              auto nominator = ton::Nominator(
                  ton::Nominator::State{
                      ton::Nominator::get_init_code(),
                      ton::Nominator::get_init_state(pool_raw_address, wallet_index)
                  });

              auto nominator_raw_address = nominator.get_address(-1);

              auto address = nominator_raw_address.rserialize();

              return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::accountAddress>(
                  address
              ));
            }
            //UNREACHABLE();
            return tonlib_api::object_ptr<tonlib_api::Object>(make_object<tonlib_api::error>(400, "Incorrect SMC type use wallet/owner/pool/nominator"));
        }(object_type);
        if (obj->get_id() != tonlib_api::error::ID) {
          Address res;
          res.address = ton::move_tl_object_as<tonlib_api::accountAddress>(obj);
          res.public_key = public_key.as_octet_string().as_slice().str();
          res.secret = td::SecureString();
          return std::move(res);
        } else {
          LOG(ERROR) << "Error during getting an address: " << to_string(obj);
        }
        Address res;
        return std::move(res);
    }

    void delete_key(td::Slice key) {
    auto r_key_i = to_key_i(key);
    if (r_key_i.is_error()) {
      td::TerminalIO::out() << "Unknown key id: [" << key << "]\n";
      return;
    }
    using tonlib_api::make_object;
    auto key_i = r_key_i.move_as_ok();
    send_query(make_object<tonlib_api::deleteKey>(
                   make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy())),

               [key = key.str()](auto r_res) {
                 if (r_res.is_error()) {
                   td::TerminalIO::out() << "Can't delete key id: [" << key << "] " << r_res.error() << "\n";
                   return;
                 }
                 td::TerminalIO::out() << "Ok\n";
               });
  }
  void export_key(std::string cmd, td::Slice key) {
    if (key.empty()) {
      dump_keys();
      td::TerminalIO::out() << "Choose public key (hex prefix or #N)";
      cont_ = [this, cmd](td::Slice key) { this->export_key(cmd, key); };
      return;
    }
    auto r_key_i = to_key_i(key);
    if (r_key_i.is_error()) {
      td::TerminalIO::out() << "Unknown key id: [" << key << "]\n";
      return;
    }
    auto key_i = r_key_i.move_as_ok();

    td::TerminalIO::out() << "Key #" << key_i << "\n"
                          << "public key: " << td::buffer_to_hex(keys_[key_i].public_key) << "\n";

    td::TerminalIO::out() << "Enter password (could be empty)";
    cont_ = [this, cmd, key = key.str(), key_i](td::Slice password) { this->export_key(cmd, key, key_i, password); };
  }

  void export_key(std::string cmd, std::string key, size_t key_i, td::Slice password) {
    using tonlib_api::make_object;
    if (cmd == "exportkey") {
      send_query(make_object<tonlib_api::exportKey>(make_object<tonlib_api::inputKeyRegular>(
                     make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy()),
                     td::SecureString(password))),
                 [this, key = std::move(key), key_i](auto r_res) {
                   if (r_res.is_error()) {
                     td::TerminalIO::out() << "Can't export key id: [" << key << "] " << r_res.error() << "\n";
                     return;
                   }
                   dump_key(key_i);
                   for (auto& word : r_res.ok()->word_list_) {
                     td::TerminalIO::out() << "    " << word.as_slice() << "\n";
                   }
                 });
    } else {
      send_query(make_object<tonlib_api::exportPemKey>(
                     make_object<tonlib_api::inputKeyRegular>(
                         make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy()),
                         td::SecureString(password)),
                     td::SecureString("cucumber")),
                 [this, key = std::move(key), key_i](auto r_res) {
                   if (r_res.is_error()) {
                     td::TerminalIO::out() << "Can't export key id: [" << key << "] " << r_res.error() << "\n";
                     return;
                   }
                   dump_key(key_i);
                   td::TerminalIO::out() << "\n" << r_res.ok()->pem_.as_slice() << "\n";
                 });
    }
  }

  void import_key(td::Slice slice, std::vector<td::SecureString> words = {}) {
    td::ConstParser parser(slice);
    while (true) {
      auto word = parser.read_word();
      if (word.empty()) {
        break;
      }
      words.push_back(td::SecureString(word));
    }
    if (words.size() < 24) {
      td::TerminalIO::out() << "Enter mnemonic words (got " << words.size() << " out of 24)";
      cont_ = [this, words = std::move(words)](td::Slice slice) mutable { this->import_key(slice, std::move(words)); };
      return;
    }
    td::TerminalIO::out() << "Enter password (could be empty)";
    cont_ = [this, words = std::move(words)](td::Slice password) mutable {
      this->import_key(std::move(words), password);
    };
  }

  void import_key(std::vector<td::SecureString> words, td::Slice password) {
    using tonlib_api::make_object;
    send_query(make_object<tonlib_api::importKey>(td::SecureString(password), td::SecureString(" test mnemonic"),
                                                  make_object<tonlib_api::exportedKey>(std::move(words))),
               [this, password = td::SecureString(password)](auto r_res) {
                 if (r_res.is_error()) {
                   td::TerminalIO::out() << "Can't import key " << r_res.error() << "\n";
                   return;
                 }
                 auto key = r_res.move_as_ok();
                 LOG(ERROR) << to_string(key);
                 KeyInfo info;
                 info.public_key = key->public_key_;
                 info.secret = std::move(key->secret_);
                 keys_.push_back(std::move(info));
                 export_key("exportkey", key->public_key_, keys_.size() - 1, std::move(password));
                 store_keys();
               });
  }
    void get_state(td::Slice key, td::Promise<td::Unit> promise) {
        TRY_RESULT_PROMISE(promise, address, to_account_address(key, false));
        using tonlib_api::make_object;
        auto address_str = address.address->account_address_;
        send_query(make_object<tonlib_api::getAccountState>(
                ton::move_tl_object_as<tonlib_api::accountAddress>(std::move(address.address))),
                   promise.wrap([address_str](auto&& state) {
                       td::TerminalIO::out() << "Address: " << address_str << "\n";
                       td::TerminalIO::out() << "Balance: "
                                             << Grams{td::narrow_cast<td::uint64>(state->balance_ * (state->balance_ > 0))}
                                             << "\n";
                       td::TerminalIO::out() << "Sync utime: " << state->sync_utime_ << "\n";
                       td::TerminalIO::out() << "transaction.LT: " << state->last_transaction_id_->lt_ << "\n";
                       td::TerminalIO::out() << "transaction.Hash: " << td::base64_encode(state->last_transaction_id_->hash_)
                                             << "\n";
                       td::TerminalIO::out() << to_string(state->account_state_);
                       return td::Unit();
                   }));
    }

    void get_address(td::Slice key, td::Promise<td::Unit> promise) {
        TRY_RESULT_PROMISE(promise, address, to_account_address(key, false));
        promise.set_value(td::Unit());
        td::TerminalIO::out() << address.address->account_address_ << "\n";
    }

    void transfer(td::ConstParser& parser, td::Slice cmd, td::Promise<td::Unit> cmd_promise) {
        bool from_file = false;
        bool force = false;
        bool use_encryption = false;
        bool use_fake_key = false;
        bool estimate_fees = false;
        if (cmd != "init") {
            td::ConstParser cmd_parser(cmd);
            cmd_parser.advance(td::Slice("transfer").size());
            while (!cmd_parser.empty()) {
                auto c = cmd_parser.peek_char();
                cmd_parser.advance(1);
                if (c == 'F') {
                    from_file = true;
                } else if (c == 'f') {
                    force = true;
                } else if (c == 'e') {
                    use_encryption = true;
                } else if (c == 'k') {
                    use_fake_key = true;
                } else if (c == 'c') {
                    estimate_fees = true;
                } else {
                    cmd_promise.set_error(td::Status::Error(PSLICE() << "Unknown suffix '" << c << "'"));
                    return;
                }
            }
        }

        auto from = parser.read_word();
        TRY_RESULT_PROMISE(cmd_promise, from_address, to_account_address(from, true));

        struct Message {
            Address to;
            td::int64 amount;
            std::string message;
        };

        std::vector<tonlib_api::object_ptr<tonlib_api::msg_message>> messages;
        auto add_message = [&](td::ConstParser& parser) {
            auto to = parser.read_word();
            auto grams = parser.read_word();
            parser.skip_whitespaces();
            auto message = parser.read_all();

            Message res;
            TRY_RESULT(address, to_account_address(to, false));
            TRY_RESULT(amount, parse_grams(grams));
            tonlib_api::object_ptr<tonlib_api::msg_Data> data;

            if (use_encryption) {
                data = tonlib_api::make_object<tonlib_api::msg_dataDecryptedText>(message.str());
            } else {
                data = tonlib_api::make_object<tonlib_api::msg_dataText>(message.str());
            }
            messages.push_back(tonlib_api::make_object<tonlib_api::msg_message>(std::move(address.address), "", amount.nano,
                                                                                std::move(data)));
            return td::Status::OK();
        };

        if (from_file) {
            TRY_RESULT_PROMISE(cmd_promise, data, td::read_file(parser.read_word().str()));
            auto lines = td::full_split(data.as_slice(), '\n');
            for (auto& line : lines) {
                td::ConstParser parser(line);
                parser.skip_whitespaces();
                if (parser.empty()) {
                    continue;
                }
                if (parser.read_word() != "SEND") {
                    TRY_STATUS_PROMISE(cmd_promise, td::Status::Error("Expected `SEND` in file"));
                }
                TRY_STATUS_PROMISE(cmd_promise, add_message(parser));
            }
        } else {
            while (parser.skip_whitespaces(), !parser.empty()) {
                TRY_STATUS_PROMISE(cmd_promise, add_message(parser));
            }
        }

        td::Slice password;  // empty by default
        using tonlib_api::make_object;
        tonlib_api::object_ptr<tonlib_api::InputKey> key =
                !from_address.secret.empty()
                ? make_object<tonlib_api::inputKeyRegular>(
                        make_object<tonlib_api::key>(from_address.public_key, from_address.secret.copy()),
                        td::SecureString(password))
                : nullptr;
        if (use_fake_key) {
            key = make_object<tonlib_api::inputKeyFake>();
        }

        bool allow_send_to_uninited = force;

        send_query(make_object<tonlib_api::createQuery>(
                std::move(key), std::move(from_address.address), 60,
                make_object<tonlib_api::actionMsg>(std::move(messages), allow_send_to_uninited)),
                   cmd_promise.send_closure(actor_id(this), &TonlibCli::transfer2, estimate_fees));
    }

    void transfer2(bool estimate_fees, td::Result<tonlib_api::object_ptr<tonlib_api::query_info>> r_info,
                   td::Promise<td::Unit> cmd_promise) {
        if (estimate_fees) {
            send_query(tonlib_api::make_object<tonlib_api::query_estimateFees>(r_info.ok()->id_, true),
                       cmd_promise.wrap([](auto&& info) {
                           td::TerminalIO::out() << "Extimated fees: " << to_string(info);
                           return td::Unit();
                       }));
        } else {
            send_query(tonlib_api::make_object<tonlib_api::query_send>(r_info.ok()->id_), cmd_promise.wrap([](auto&& info) {
                td::TerminalIO::out() << "Transfer sent: " << to_string(info);
                return td::Unit();
            }));
        }
    }



    void init_account(td::Slice key, td::Slice smc_type, td::Slice wallet_idx) {


      auto r_key_i = to_key_i(key);
      if (r_key_i.is_error()) {
        td::TerminalIO::out() << "Unknown key id: [" << key << "]\n";
        return;
      }
      auto key_i = r_key_i.move_as_ok();

      td::TerminalIO::out() << "Key #" << key_i << "\n"
                            << "public key: " << td::buffer_to_hex(keys_[key_i].public_key) << "\n";

      td::TerminalIO::out() << "Enter password (could be empty)";

      auto wallet_id = td::to_integer_safe<td::uint32>(wallet_idx);

      td::uint32 wallet_index=0;

      if( !wallet_id.is_error() ){
        wallet_index = wallet_id.move_as_ok();
      }


      cont_ = [this, key_i, smc_type, wallet_index](td::Slice password) { this->init_account(key_i, password, smc_type, wallet_index); };
    }

    void check(td::Status status) {
      if (status.is_error()) {
        cont_.set_error(std::move(status));
        return stop();
      }
    }



    void init_account(size_t key_i, td::Slice password, td::Slice smc_type, td::uint32 wallet_idx) {
      using tonlib_api::make_object;
      //td::Promise<td::Unit> promise;


      auto pk = tonlib::TonlibClient::static_request(make_object<tonlib_api::exportPemKey>(
          make_object<tonlib_api::inputKeyRegular>(
              make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy()),
              td::SecureString(password)),
          td::SecureString("cucumber"))
      );


      send_query(make_object<tonlib_api::exportPemKey>(
          make_object<tonlib_api::inputKeyRegular>(
              make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy()),
                    td::SecureString(password)),
                    td::SecureString("cucumber")),
                 [this, key_i, smc_type, wallet_idx](auto r_res) {

                   if (r_res.is_error()) {
                     td::TerminalIO::out() << "Can't export key id: [" << key_i << "] " << r_res.error() << "\n";
                     return;
                   }
                   dump_key(key_i);
                   td::TerminalIO::out() << "\n" << r_res.ok()->pem_.as_slice() << "\n";

                   auto private_key = td::Ed25519::PrivateKey::from_pem(r_res.ok()->pem_.as_slice(), "cucumber" ).move_as_ok();
                   td::TerminalIO::out() << "Public : \n" << private_key.get_public_key().move_as_ok().as_octet_string() << "\n";
                   auto public_key = private_key.get_public_key().move_as_ok();

                   td::Ref<vm::Cell> init_message;
                   td::Ref<vm::Cell> init_data;
                   td::Ref<vm::Cell> init_code;

                   if( smc_type == "wallet"){
                     td::uint32 wallet_id = wallet_idx != 0 ? wallet_idx : wallet_id_;

                     init_code = ton::WalletV3::get_init_code();
                     init_data = ton::WalletV3::get_init_data( public_key, wallet_id );
                     init_message = ton::WalletV3::get_init_message( private_key, wallet_id );
                   }
                   if( smc_type == "owner") {
                     init_code = ton::WalletV3::get_init_code();
                     init_data = ton::WalletV3::get_init_data(public_key, 0);
                     init_message = ton::WalletV3::get_init_message(private_key, 0);
                   }
                   if( smc_type == "pool") {
                       auto owner_init_code = ton::WalletV3::get_init_code();
                       auto owner_init_data = ton::WalletV3::get_init_data(public_key, 0);
                       auto owner_address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(owner_init_code, owner_init_data));

                       init_code = ton::StakingPool::get_init_code();
                       init_data = ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_address);
                       auto pool_address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(init_code, init_data));

                       std::vector<block::StdAddress> nominators;
                       auto nominator_code = ton::Nominator::get_init_code();

                       for(int a = 1; a < 10 ; a ++ ){
                         auto nominator_data = ton::Nominator::get_init_data(pool_address, a);
                         auto nominator_address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(nominator_code, nominator_data));
                         nominators.push_back(nominator_address);
                       }
                       init_message = ton::StakingPool::get_init_message(pool_address, &nominators);
                   }
                     if( smc_type == "nominator" && wallet_idx != 0) {
                       auto owner_init_code = ton::WalletV3::get_init_code();
                       auto owner_init_data = ton::WalletV3::get_init_data(public_key, 0);
                       auto owner_address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(owner_init_code, owner_init_data));

                       init_code = ton::StakingPool::get_init_code();
                       init_data = ton::StakingPool::get_init_data(1000, 10000, 500, 500, owner_address);
                       auto pool_address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(init_code, init_data));

                       std::vector<block::StdAddress> nominators;
                       auto nominator_code = ton::Nominator::get_init_code();

                       auto nominator_data = ton::Nominator::get_init_data(pool_address, wallet_idx);
                     }


                   if( !init_message.is_null() && !init_code.is_null() && !init_data.is_null()  ){

                    auto address = ton::GenericAccount::get_address(ton::basechainId, ton::GenericAccount::get_init_state(init_code, init_data)).rserialize();
                    td::TerminalIO::out() << "Address : " << address << "\n";

                    auto init_code_s = vm::std_boc_serialize(init_code).move_as_ok().as_slice().str();
                    auto init_data_s = vm::std_boc_serialize(init_data).move_as_ok().as_slice().str();
                    auto init_message_s = vm::std_boc_serialize(init_message).move_as_ok().as_slice().str();


                     send_query(tonlib_api::make_object<tonlib_api::raw_createQuery>(
                         tonlib_api::make_object<tonlib_api::accountAddress>(address), init_code_s, init_data_s, init_message_s),[this](auto&& res){
                               td::TerminalIO::out() << "Query was built \n";
                               send_query(tonlib_api::make_object<tonlib_api::query_send>( res.ok()->id_),[](auto &&res){
                                   td::TerminalIO::out() << "Query was sent \n";
                               });
                         });
                  } else{
                          td::TerminalIO::out() << "Cannot initialize " << smc_type << "\n";
                  }
              });
    }


    void get_hints(td::Slice prefix) {
    using tonlib_api::make_object;
    auto obj = tonlib::TonlibClient::static_request(make_object<tonlib_api::getBip39Hints>(prefix.str()));
    if (obj->get_id() == tonlib_api::error::ID) {
      return;
    }
    td::TerminalIO::out() << to_string(obj);
  }


    void pool_command(td::Slice key, td::Slice command, td::Slice param1, td::Slice param2) {
      if (key.empty()) {
        dump_keys();
        td::TerminalIO::out() << "Choose public key (hex prefix or #N)" << "\n";
        cont_ = [this, command, param1, param2](td::Slice key) { this->pool_command(key, command, param1, param2); };
        return;
      }
      if (command.empty()) {
        td::TerminalIO::out() << "Choose command sns (set nominator status) / pr (close period)" << "\n";
        cont_ = [this, key, param1, param2](td::Slice command) { this->pool_command(key, command, param1, param2); };
        return;
      }

      auto r_key_i = to_key_i(key);
      if (r_key_i.is_error()) {
        td::TerminalIO::out() << "Unknown key id: [" << key << "]\n";
        return;
      }
      auto key_i = r_key_i.move_as_ok();

      td::TerminalIO::out() << "Key #" << key_i << "\n"
                            << "public key: " << td::buffer_to_hex(keys_[key_i].public_key) << "\n";

      td::TerminalIO::out() << "Enter password (could be empty)";



      cont_ = [this, key_i, command, param1, param2](td::Slice password) {

          send_query(tonlib_api::make_object<tonlib_api::exportPemKey>(
              tonlib_api::make_object<tonlib_api::inputKeyRegular>(
                  tonlib_api::make_object<tonlib_api::key>(keys_[key_i].public_key, keys_[key_i].secret.copy()),
                  td::SecureString(password)),
              td::SecureString("cucumber")),
                     [this, key_i, command, param1, param2](auto r_res) {

                         if (r_res.is_error()) {
                           td::TerminalIO::out() << "Can't export key id: [" << key_i << "] " << r_res.error() << "\n";
                           return;
                         }
                         dump_key(key_i);
                         td::TerminalIO::out() << "\n" << r_res.ok()->pem_.as_slice() << "\n";

                         if (command == "sns" && param1.empty()) {
                           this->pool_command_sns( r_res.move_as_ok()->pem_.as_slice().str(), param1, param2);
                         }

                     });
      };

      td::TerminalIO::out() << "Incorrect input" << key << " " << command << "\n";

    }




    void pool_command_sns(td::string private_key, td::Slice nominator_id, td::Slice status) {



      if (nominator_id.empty()) {
        td::TerminalIO::out() << "Please set nominator index (1-..)";
        cont_ = [this, private_key, status](td::Slice nominator_id) {
          this->pool_command_sns(private_key, nominator_id, status);
        };
        return;
      }
      if (status.empty()) {
        td::TerminalIO::out() << "Please select status (1-..)";
        cont_ = [this, private_key,  nominator_id ](td::Slice status) { this->pool_command_sns(private_key,  nominator_id, status); };
        return;
      }
      auto pk = td::Ed25519::PrivateKey::from_pem(private_key,"cucumber").move_as_ok();
      td::TerminalIO::out() << "Public : \n"
                            << pk.get_public_key().move_as_ok().as_octet_string() << "\n";
      auto pub_key = pk.get_public_key().move_as_ok();
      auto nominator_i = td::to_integer_safe<td::uint32>(nominator_id);
      auto status_i = td::to_integer_safe<td::uint32>(status);

      td::uint32 wallet_index=0;

      if( nominator_i.is_error() || status_i.is_error() ){
        td::TerminalIO::out() << "Incorrect input" << "\n";
      }
      auto key_i = to_key_i(pub_key.as_octet_string().as_slice());

      auto owner_address = this->to_account_address(pub_key, td::Slice("owner"), td::Slice("0")).move_as_ok();
      auto pool_address = this->to_account_address(pub_key, td::Slice("pool"), td::Slice("0")).move_as_ok();
      auto nominator_address = this->to_account_address(pub_key, td::Slice("nominator"), td::Slice(std::to_string(nominator_i.ok()) )).move_as_ok();

      td::TerminalIO::out() << "Owner address : " << owner_address.address->account_address_ << "\n";
      td::TerminalIO::out() << "Pool address : " << pool_address.address->account_address_ << "\n";
      td::TerminalIO::out() << "Nominator address : " << nominator_address.address->account_address_ << "\n";

      auto request = ton::StakingPool::set_nominator_status_request(block::StdAddress::parse(nominator_address.address->account_address_).move_as_ok(), status_i.ok(), 0);
      /*
      send_request(
          block::StdAddress::parse(owner_address.address->account_address_).move_as_ok(),
          block::StdAddress::parse(pool_address.address->account_address_).move_as_ok(),
          pk, 100000, request );
      */
      send_request(
              block::StdAddress::parse(owner_address.address->account_address_).move_as_ok(),
              //block::StdAddress::parse(pool_address.address->account_address_).move_as_ok(),
              block::StdAddress::parse("EQASIbkPphLDxa_YntBbvDwo7JWUJgl8q4LDtM76wCtN_T4O").move_as_ok(),
              pk, 100000, request );

    }

    void send_request( block::StdAddress wallet, block::StdAddress to, td::Ed25519::PrivateKey& private_key0, td::int64 grams, td::Ref<vm::Cell> request ) {
      auto wallet_account = tonlib_api::make_object<tonlib_api::accountAddress>(wallet.rserialize());



      auto private_key = td::Ed25519::PrivateKey(private_key0.as_octet_string().copy());
      td::TerminalIO::out() << private_key.as_pem(td::Slice("cucumber")).move_as_ok();

      auto message0 = ton::WalletV3::make_message(private_key, 0, 1, 60, grams, vm::std_boc_serialize(request).move_as_ok().as_slice(), to );
      auto message1 = ton::WalletV3::make_message(private_key0, 0, 1, 60, grams, vm::std_boc_serialize(request).move_as_ok().as_slice(), to );

      auto k1 = private_key0.as_pem("asd").move_as_ok();
      auto k2 = private_key.as_pem("asd").move_as_ok();

      td::TerminalIO::out() << k1 << "\n";
      td::TerminalIO::out() << k2 << "\n";


      send_query( tonlib_api::make_object<tonlib_api::smc_load>( std::move(wallet_account) ), [this, request, grams, wallet = block::StdAddress(wallet), private_key = td::Ed25519::PrivateKey(private_key.as_octet_string()), to=block::StdAddress(to)](auto smc_info){
          td::TerminalIO::out() << "SMC loaded: " << "\n";
          send_query( tonlib_api::make_object<tonlib_api::smc_getData>(smc_info.ok()->id_ ), [this, request, grams, wallet = block::StdAddress(wallet), private_key = td::Ed25519::PrivateKey(private_key.as_octet_string()), to=block::StdAddress(to)](auto smc_data){
              td::TerminalIO::out() << "Data loaded: " << "\n";
              auto owner_wallet = ton::WalletV3( ton::StakingSmartContract::State{
                  ton::WalletV3::get_init_code(),
                  vm::std_boc_deserialize(smc_data.ok()->bytes_).move_as_ok()
              });
              auto seqno = owner_wallet.get_seqno().move_as_ok();
              auto wallet_id = owner_wallet.get_wallet_id().move_as_ok();

              td::TerminalIO::out() << "Seqno: " << seqno << "\n";

              td::TerminalIO::out() << private_key.as_pem(td::Slice("cucumber")).move_as_ok();


              //auto message = ton::WalletV3::make_message(private_key, wallet_id, seqno+1, 60, grams, vm::std_boc_serialize(request).move_as_ok().as_slice(), to );

              auto message = ton::WalletV3::make_a_gift_message(private_key, wallet_id, seqno, 0x5EFFDFB64, grams, td::Slice("111"), to );


              auto message_s = vm::std_boc_serialize(message).move_as_ok().as_slice().str();

              auto res = owner_wallet.send_external_message( message );
              td::TerminalIO::out() << "res: " << res.code << "\n";
              td::TerminalIO::out() << "addr from: " << wallet.rserialize() << "\n";
              td::TerminalIO::out() << "addr to: " << to.rserialize() << "\n";
              td::TerminalIO::out() << "msg : " << message_s << "\n";


              send_query(tonlib_api::make_object<tonlib_api::raw_createQuery>(
                  tonlib_api::make_object<tonlib_api::accountAddress>(wallet.rserialize()), "", "", message_s),[this ](auto&& res){
                  td::TerminalIO::out() << "Query was built \n";
                  send_query(tonlib_api::make_object<tonlib_api::query_send>( res.ok()->id_),[](auto &&res){
                      td::TerminalIO::out() << "Query was sent \n";
                  });
              });
              /*
              send_query(tonlib_api::make_object<tonlib_api::raw_sendMessage>(
                  tonlib_api::make_object<tonlib_api::accountAddress>(wallet.rserialize()), "", "", message_s),[this ](auto&& res){
                  td::TerminalIO::out() << "Query was built \n";
                  send_query(tonlib_api::make_object<tonlib_api::query_send>( res.ok()->id_),[](auto &&res){
                      td::TerminalIO::out() << "Query was sent \n";
                  });
              });
               */



          });
      });
    }



};




int main(int argc, char* argv[]) {
  using tonlib_api::make_object;
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();

  td::OptionsParser p;
  TonlibCli::Options options;
  p.set_description("console for validator for TON Blockchain");
  p.add_option('h', "help", "prints_help", [&]() {
    std::cout << (PSLICE() << p).c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('r', "disable-readline", "disable readline", [&]() {
    options.enable_readline = false;
    return td::Status::OK();
  });
  p.add_option('R', "enable-readline", "enable readline", [&]() {
    options.enable_readline = true;
    return td::Status::OK();
  });
  p.add_option('D', "directory", "set keys directory", [&](td::Slice arg) {
    options.key_dir = arg.str();
    return td::Status::OK();
  });
  p.add_option('M', "in-memory", "store keys only in-memory", [&]() {
    options.in_memory = true;
    return td::Status::OK();
  });
  p.add_option('E', "execute", "execute one command", [&](td::Slice arg) {
    options.one_shot = true;
    options.cmd = arg.str();
    return td::Status::OK();
  });
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    auto verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 20) ? td::Status::OK() : td::Status::Error("verbosity must be 0..20");
  });
  p.add_option('C', "config-force", "set lite server config, drop config related blockchain cache", [&](td::Slice arg) {
    TRY_RESULT(data, td::read_file_str(arg.str()));
    options.config = std::move(data);
    options.ignore_cache = true;
    return td::Status::OK();
  });
  p.add_option('c', "config", "set lite server config", [&](td::Slice arg) {
    TRY_RESULT(data, td::read_file_str(arg.str()));
    options.config = std::move(data);
    return td::Status::OK();
  });
  p.add_option('N', "config-name", "set lite server config name", [&](td::Slice arg) {
    options.name = arg.str();
    return td::Status::OK();
  });
  p.add_option('n', "use-callbacks-for-network", "do not use this", [&]() {
    options.use_callbacks_for_network = true;
    return td::Status::OK();
  });
  p.add_option('W', "wallet-version", "do not use this", [&](td::Slice arg) {
    options.wallet_version = td::to_integer<td::int32>(arg);
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  td::actor::Scheduler scheduler({2});
  scheduler.run_in_context([&] { td::actor::create_actor<TonlibCli>("console", options).release(); });
  scheduler.run();
  return 0;
}

