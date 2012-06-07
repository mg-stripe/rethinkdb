#ifndef CLUSTERING_ADMINISTRATION_MAIN_SERVE_HPP_
#define CLUSTERING_ADMINISTRATION_MAIN_SERVE_HPP_

#include "clustering/administration/metadata.hpp"
#include "clustering/administration/persist.hpp"

/* This has been factored out from `command_line.hpp` because it takes a very
long time to compile. */

bool serve(const std::string &filepath, metadata_persistence::persistent_file_t *persistent_file, const std::set<peer_address_t> &joins, int port, DEBUG_ONLY(int port_offset,) int client_port, machine_id_t machine_id, const cluster_semilattice_metadata_t &semilattice_metadata, std::string web_assets, signal_t *stop_cond);
bool serve_proxy(const std::string &logfilepath, const std::set<peer_address_t> &joins, DEBUG_ONLY(int port_offset,) int client_port, machine_id_t machine_id, const cluster_semilattice_metadata_t &semilattice_metadata, std::string web_assets, signal_t *stop_cond);

#endif /* CLUSTERING_ADMINISTRATION_MAIN_SERVE_HPP_ */
