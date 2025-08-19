#include "arg.hpp"
#include <argparse/argparse.hpp>

using namespace argparse;

void parse_args(int argc, char **argv, urp::EndpointConfig &cfg) {
  ArgumentParser parser("server");
  parser.add_argument("-p", "--port")
      .help("Port ID")
      .default_value(0)
      .action(
          [&](const std::string &value) { cfg.port_id = std::stoi(value); });

  parser.add_argument("-tx", "--tx-burst")
      .help("TX burst size")
      .default_value(128)
      .action([&](const std::string &value) {
        cfg.tx_burst_size = std::stoi(value);
      });

  parser.add_argument("-rx", "--rx-burst")
      .help("RX burst size")
      .default_value(128)
      .action([&](const std::string &value) {
        cfg.rx_burst_size = std::stoi(value);
      });

  try {
    parser.parse_args(argc, argv);
  } catch (const std::runtime_error &err) {
    std::cerr << err.what() << std::endl;
    exit(1);
  }
}