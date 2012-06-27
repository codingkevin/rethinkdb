#define __STDC_FORMAT_MACROS
#include "clustering/administration/cli/admin_cluster_link.hpp"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include <map>
#include <stdexcept>

#include "errors.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/ptr_container/ptr_map.hpp>

#include "arch/io/network.hpp"
#include "clustering/administration/cli/key_parsing.hpp"
#include "clustering/administration/suggester.hpp"
#include "clustering/administration/metadata_change_handler.hpp"
#include "clustering/administration/main/watchable_fields.hpp"
#include "rpc/connectivity/multiplexer.hpp"
#include "rpc/semilattice/view.hpp"
#include "rpc/semilattice/semilattice_manager.hpp"
#include "memcached/protocol_json_adapter.hpp"
#include "do_on_thread.hpp"
#include "perfmon/perfmon.hpp"
#include "perfmon/archive.hpp"

std::string admin_cluster_link_t::peer_id_to_machine_name(const std::string& peer_id) {
    std::string result(peer_id);

    if (is_uuid(peer_id)) {
        peer_id_t peer(str_to_uuid(peer_id));

        if (peer == connectivity_cluster.get_me()) {
            result.assign("admin_cli");
        } else {
            std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();
            std::map<peer_id_t, cluster_directory_metadata_t>::iterator i = directory.find(peer);

            if (i != directory.end()) {
                try {
                    result = get_info_from_id(uuid_to_str(i->second.machine_id))->name;
                } catch (...) {
                }
            }
        }
    }

    return result;
}

void admin_cluster_link_t::admin_stats_to_table(const std::string& machine,
                                                const std::string& prefix,
                                                const perfmon_result_t& stats,
                                                std::vector<std::vector<std::string> >& table) {
    std::vector<std::string> delta;

    if (stats.is_string()) {
        // TODO: this should only happen in the case of an empty stats, which shouldn't really happen
        delta.push_back(machine);
        delta.push_back("-");
        delta.push_back("-");
        delta.push_back("-");
        table.push_back(delta);
    } else if (stats.is_map()) {
        for (perfmon_result_t::const_iterator i = stats.begin(); i != stats.end(); ++i) {
            if (i->second->is_string()) {
                delta.clear();
                delta.push_back(machine);
                delta.push_back(prefix);
                delta.push_back(i->first);
                delta.push_back(*i->second->get_string());
                table.push_back(delta);
            } else {
                std::string postfix(i->first);
                // Try to convert any uuids to a name, if that fails, ignore
                try {
                    uuid_t temp = str_to_uuid(postfix);
                    postfix = get_info_from_id(uuid_to_str(temp))->name;
                } catch (...) {
                    postfix = peer_id_to_machine_name(postfix);
                }
                admin_stats_to_table(machine, prefix.empty() ? postfix : (prefix + "/" + postfix), *i->second, table);
            }
        }
    }
}

std::string admin_value_to_string(const memcached_protocol_t::region_t& region) {
    // TODO(sam): I don't know what is the appropriate sort of thing for an admin_value_to_string call.
    return strprintf("%" PRIu64 ":%" PRIu64 ":%s", region.beg, region.end, key_range_to_cli_str(region.inner).c_str());
}

std::string admin_value_to_string(const mock::dummy_protocol_t::region_t& region) {
    return mock::region_to_debug_str(region);
}

std::string admin_value_to_string(int value) {
    return strprintf("%i", value);
}

std::string admin_value_to_string(const uuid_t& uuid) {
    return uuid_to_str(uuid);
}

std::string admin_value_to_string(const std::string& str) {
    return "\"" + str + "\"";
}

std::string admin_value_to_string(const std::map<uuid_t, int>& value) {
    std::string result;
    size_t count = 0;
    for (std::map<uuid_t, int>::const_iterator i = value.begin(); i != value.end(); ++i) {
        ++count;
        result += strprintf("%s: %i%s", uuid_to_str(i->first).c_str(), i->second, count == value.size() ? "" : ", ");
    }
    return result;
}

std::string admin_value_to_string(const std::set<mock::dummy_protocol_t::region_t>& value) {
    std::string result;
    size_t count = 0;
    for (std::set<mock::dummy_protocol_t::region_t>::const_iterator i = value.begin(); i != value.end(); ++i) {
        ++count;
        result += strprintf("%s%s", admin_value_to_string(*i).c_str(), count == value.size() ? "" : ", ");
    }
    return result;
}

std::string admin_value_to_string(const std::set<hash_region_t<key_range_t> >& value) {
    std::string result;
    size_t count = 0;
    for (std::set<hash_region_t<key_range_t> >::const_iterator i = value.begin(); i != value.end(); ++i) {
        ++count;
        result += strprintf("%s%s", admin_value_to_string(*i).c_str(), count == value.size() ? "" : ", ");
    }
    return result;
}

template <class protocol_t>
std::string admin_value_to_string(const region_map_t<protocol_t, uuid_t>& value) {
    std::string result;
    size_t count = 0;
    for (typename region_map_t<protocol_t, uuid_t>::const_iterator i = value.begin(); i != value.end(); ++i) {
        ++count;
        result += strprintf("%s: %s%s", admin_value_to_string(i->first).c_str(), uuid_to_str(i->second).c_str(), count == value.size() ? "" : ", ");
    }
    return result;
}

template <class protocol_t>
std::string admin_value_to_string(const region_map_t<protocol_t, std::set<uuid_t> >& value) {
    std::string result;
    size_t count = 0;
    for (typename region_map_t<protocol_t, std::set<uuid_t> >::const_iterator i = value.begin(); i != value.end(); ++i) {
        ++count;
        //TODO: print more detail
        result += strprintf("%s: %ld machine%s%s", admin_value_to_string(i->first).c_str(), i->second.size(), i->second.size() == 1 ? "" : "s", count == value.size() ? "" : ", ");
    }
    return result;
}

void admin_print_table(const std::vector<std::vector<std::string> >& table) {
    std::vector<int> column_widths;

    if (table.size() == 0) {
        return;
    }

    // Verify that the vectors are consistent size
    for (size_t i = 1; i < table.size(); ++i) {
        if (table[i].size() != table[0].size()) {
            throw admin_cluster_exc_t("unexpected error when printing table");
        }
    }

    // Determine the maximum size of each column
    for (size_t i = 0; i < table[0].size(); ++i) {
        int max = table[0][i].length();

        for (size_t j = 1; j < table.size(); ++j) {
            if ((int)table[j][i].length() > max) {
                max = table[j][i].length();
            }
        }

        column_widths.push_back(max);
    }

    // Print out each line, spacing each column
    for (size_t i = 0; i < table.size(); ++i) {
        for (size_t j = 0; j < table[i].size(); ++j) {
            printf("%-*s", column_widths[j] + 2, table[i][j].c_str());
        }
        printf("\n");
    }
}

// Truncate a uuid for easier user-interface
std::string admin_cluster_link_t::truncate_uuid(const uuid_t& uuid) {
    if (uuid.is_nil()) {
        return std::string("none");
    } else {
        return uuid_to_str(uuid).substr(0, uuid_output_length);
    }
}

admin_cluster_link_t::admin_cluster_link_t(const std::set<peer_address_t> &joins, int client_port, signal_t *interruptor) :
    local_issue_tracker(),
    log_writer("./rethinkdb_log_file", &local_issue_tracker), // TODO: come up with something else for this file
    connectivity_cluster(),
    message_multiplexer(&connectivity_cluster),
    mailbox_manager_client(&message_multiplexer, 'M'),
    mailbox_manager(&mailbox_manager_client),
    stat_manager(&mailbox_manager),
    log_server(&mailbox_manager, &log_writer),
    mailbox_manager_client_run(&mailbox_manager_client, &mailbox_manager),
    semilattice_manager_client(&message_multiplexer, 'S'),
    semilattice_manager_cluster(&semilattice_manager_client, cluster_semilattice_metadata_t()),
    semilattice_manager_client_run(&semilattice_manager_client, &semilattice_manager_cluster),
    semilattice_metadata(semilattice_manager_cluster.get_root_view()),
    metadata_change_handler(&mailbox_manager, semilattice_metadata),
    directory_manager_client(&message_multiplexer, 'D'),
    our_directory_metadata(cluster_directory_metadata_t(connectivity_cluster.get_me().get_uuid(), get_ips(), stat_manager.get_address(), metadata_change_handler.get_request_mailbox_address(), log_server.get_business_card(), ADMIN_PEER)),
    directory_read_manager(connectivity_cluster.get_connectivity_service()),
    directory_write_manager(&directory_manager_client, our_directory_metadata.get_watchable()),
    directory_manager_client_run(&directory_manager_client, &directory_read_manager),
    message_multiplexer_run(&message_multiplexer),
    connectivity_cluster_run(&connectivity_cluster, 0, &message_multiplexer_run, client_port),
    admin_tracker(semilattice_metadata, directory_read_manager.get_root_view()),
    initial_joiner(&connectivity_cluster, &connectivity_cluster_run, joins, 5000)
{
    wait_interruptible(initial_joiner.get_ready_signal(), interruptor);
    if (!initial_joiner.get_success()) {
        throw admin_cluster_exc_t("failed to join cluster");
    }
}

admin_cluster_link_t::~admin_cluster_link_t() {
    clear_metadata_maps();
}

metadata_change_handler_t<cluster_semilattice_metadata_t>::request_mailbox_t::address_t admin_cluster_link_t::choose_sync_peer() {
    std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();

    for (std::map<peer_id_t, cluster_directory_metadata_t>::iterator i = directory.begin(); i != directory.end(); ++i) {
        if (i->second.peer_type == SERVER_PEER) {
            change_request_id = i->second.machine_id;
            sync_peer_id = i->first;
            return i->second.semilattice_change_mailbox;
        }
    }
    throw admin_cluster_exc_t("no reachable server found in the cluster");
}

void admin_cluster_link_t::update_metadata_maps() {
    clear_metadata_maps();

    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    add_subset_to_maps("machines", cluster_metadata.machines.machines);
    add_subset_to_maps("datacenters", cluster_metadata.datacenters.datacenters);
    add_subset_to_maps("dummy_namespaces", cluster_metadata.dummy_namespaces.namespaces);
    add_subset_to_maps("memcached_namespaces", cluster_metadata.memcached_namespaces.namespaces);
}

void admin_cluster_link_t::clear_metadata_maps() {
    // All metadata infos will be in the name_map and uuid_map exactly one each
    for (std::map<std::string, metadata_info_t*>::iterator i = uuid_map.begin(); i != uuid_map.end(); ++i) {
        delete i->second;
    }

    name_map.clear();
    uuid_map.clear();
}

template <class T>
void admin_cluster_link_t::add_subset_to_maps(const std::string& base, T& data_map) {
    for (typename T::const_iterator i = data_map.begin(); i != data_map.end(); ++i) {
        if (i->second.is_deleted()) {
            continue;
        }

        metadata_info_t* info = new metadata_info_t;
        info->uuid = i->first;
        std::string uuid_str = uuid_to_str(i->first);
        info->path.push_back(base);
        info->path.push_back(uuid_str);

        if (!i->second.get().name.in_conflict()) {
            info->name = i->second.get().name.get();
            name_map.insert(std::pair<std::string, metadata_info_t*>(info->name, info));
        }
        uuid_map.insert(std::pair<std::string, metadata_info_t*>(uuid_str, info));
    }
}

void admin_cluster_link_t::sync_from() {
    try {
        cond_t interruptor;
        std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();

        if (sync_peer_id.is_nil() || directory.count(sync_peer_id) == 0) {
            choose_sync_peer();
        }
        
        semilattice_metadata->sync_from(sync_peer_id, &interruptor);
    } catch (sync_failed_exc_t& ex) {
        throw admin_no_connection_exc_t("connection lost to cluster");
    } catch (admin_cluster_exc_t& ex) {
        // No sync peer, continue on with old data
    }

    semilattice_metadata = semilattice_manager_cluster.get_root_view();
    update_metadata_maps();
}

std::vector<std::string> admin_cluster_link_t::get_ids_internal(const std::string& base, const std::string& path) {
    std::vector<std::string> results;

    // TODO: check for uuid collisions, give longer completions
    // Build completion values
    for (std::map<std::string, metadata_info_t*>::iterator i = uuid_map.lower_bound(base);
         i != uuid_map.end() && i->first.find(base) == 0; ++i) {
        if (path.empty() || i->second->path[0] == path) {
            results.push_back(i->first.substr(0, uuid_output_length));
        }
    }

    for (std::map<std::string, metadata_info_t*>::iterator i = name_map.lower_bound(base);
         i != name_map.end() && i->first.find(base) == 0; ++i) {
        if (path.empty() || i->second->path[0] == path) {
            results.push_back(i->first);
        }
    }

    return results;
}

std::vector<std::string> admin_cluster_link_t::get_ids(const std::string& base) {
    return get_ids_internal(base, "");
}

std::vector<std::string> admin_cluster_link_t::get_machine_ids(const std::string& base) {
    return get_ids_internal(base, "machines");
}

std::vector<std::string> admin_cluster_link_t::get_namespace_ids(const std::string& base) {
    std::vector<std::string> namespaces = get_ids_internal(base, "dummy_namespaces");
    std::vector<std::string> delta = get_ids_internal(base, "memcached_namespaces");
    std::copy(delta.begin(), delta.end(), std::back_inserter(namespaces));
    return namespaces;
}

std::vector<std::string> admin_cluster_link_t::get_datacenter_ids(const std::string& base) {
    return get_ids_internal(base, "datacenters");
}

std::vector<std::string> admin_cluster_link_t::get_conflicted_ids(const std::string& base UNUSED) {
    std::set<std::string> unique_set;
    std::vector<std::string> results;

    std::list<clone_ptr_t<vector_clock_conflict_issue_t> > conflicts = admin_tracker.vector_clock_conflict_issue_tracker.get_vector_clock_issues();

    for (std::list<clone_ptr_t<vector_clock_conflict_issue_t> >::iterator i = conflicts.begin(); i != conflicts.end(); ++i) {
        unique_set.insert(uuid_to_str(i->get()->object_id));
    }

    for (std::set<std::string>::iterator i = unique_set.begin(); i != unique_set.end(); ++i) {
        if (i->find(base) == 0) {
            results.push_back(i->substr(0, uuid_output_length));
        }
    }

    for (std::set<std::string>::iterator i = unique_set.begin(); i != unique_set.end(); ++i) {
        std::map<std::string, metadata_info_t*>::iterator info = uuid_map.find(*i);
        if (info != uuid_map.end() && info->second->name.find(base) == 0) {
            results.push_back(info->second->name);
        }
    }

    return results;
}

boost::shared_ptr<json_adapter_if_t<namespace_metadata_ctx_t> > admin_cluster_link_t::traverse_directory(const std::vector<std::string>& path, namespace_metadata_ctx_t& json_ctx, cluster_semilattice_metadata_t& cluster_metadata)
{
    // as we traverse the json sub directories this will keep track of where we are
    boost::shared_ptr<json_adapter_if_t<namespace_metadata_ctx_t> > json_adapter_head(new json_adapter_t<cluster_semilattice_metadata_t, namespace_metadata_ctx_t>(&cluster_metadata));

    std::vector<std::string>::const_iterator it = path.begin();

    // Traverse through the subfields until we're done with the url
    while (it != path.end()) {
        json_adapter_if_t<namespace_metadata_ctx_t>::json_adapter_map_t subfields = json_adapter_head->get_subfields(json_ctx);
        if (subfields.find(*it) == subfields.end()) {
            throw std::runtime_error("path not found: " + *it);
        }
        json_adapter_head = subfields[*it];
        it++;
    }

    return json_adapter_head;
}

admin_cluster_link_t::metadata_info_t* admin_cluster_link_t::get_info_from_id(const std::string& id) {
    // Names must be an exact match, but uuids can be prefix substrings
    if (name_map.count(id) == 0) {
        std::map<std::string, metadata_info_t*>::iterator item = uuid_map.lower_bound(id);

        if (id.length() < minimum_uuid_substring) {
            throw admin_parse_exc_t("identifier not found, too short to specify a uuid: " + id);
        }

        if (item == uuid_map.end() || item->first.find(id) != 0) {
            throw admin_parse_exc_t("identifier not found: " + id);
        }

        // Make sure that the found id is unique
        ++item;
        if (item != uuid_map.end() && item->first.find(id) == 0) {
            throw admin_cluster_exc_t("uuid not unique: " + id);
        }

        return uuid_map.lower_bound(id)->second;
    } else if (name_map.count(id) != 1) {
        std::string exception_info(strprintf("'%s' not unique, possible objects:", id.c_str()));

        for (std::map<std::string, metadata_info_t*>::iterator item = name_map.lower_bound(id);
             item != name_map.end() && item->first == id; ++item) {
            if (item->second->path[0] == "datacenters") {
                exception_info += strprintf("\ndatacenter    %s", uuid_to_str(item->second->uuid).substr(0, uuid_output_length).c_str());
            } else if (item->second->path[0] == "dummy_namespaces") {
                exception_info += strprintf("\nnamespace (d) %s", uuid_to_str(item->second->uuid).substr(0, uuid_output_length).c_str());
            } else if (item->second->path[0] == "memcached_namespaces") {
                exception_info += strprintf("\nnamespace (m) %s", uuid_to_str(item->second->uuid).substr(0, uuid_output_length).c_str());
            } else if (item->second->path[0] == "machines") {
                exception_info += strprintf("\nmachine       %s", uuid_to_str(item->second->uuid).substr(0, uuid_output_length).c_str());
            } else {
                exception_info += strprintf("\nunknown       %s", uuid_to_str(item->second->uuid).substr(0, uuid_output_length).c_str());
            }
        }
        throw admin_cluster_exc_t(exception_info);
    }

    return name_map.find(id)->second;
}

datacenter_id_t get_machine_datacenter(const std::string& id, const machine_id_t& machine, cluster_semilattice_metadata_t& cluster_metadata) {
    machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.find(machine);

    if (i == cluster_metadata.machines.machines.end()) {
        throw admin_cluster_exc_t("unexpected error, machine not found: " + uuid_to_str(machine));
    }

    if (i->second.is_deleted()) {
        throw admin_cluster_exc_t("unexpected error, machine is deleted: " + uuid_to_str(machine));
    }

    if (i->second.get().datacenter.in_conflict()) {
        throw admin_cluster_exc_t("datacenter is in conflict for machine " + id);
    }

    return i->second.get().datacenter.get();
}

void admin_cluster_link_t::do_admin_pin_shard(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::string ns(data.params["namespace"][0]);
    std::vector<std::string> ns_path(get_info_from_id(ns)->path);
    std::string shard_str(data.params["key"][0]);
    std::string primary;
    std::vector<std::string> secondaries;

    if (ns_path[0] == "dummy_namespaces") {
        throw admin_cluster_exc_t("pinning not supported for dummy namespaces");
    } else if(ns_path[0] != "memcached_namespaces") {
        throw admin_parse_exc_t("object is not a namespace: " + ns);
    }

    if (data.params.count("master") == 1) {
        primary.assign(data.params["master"][0]);
    }

    if (data.params.count("replicas") != 0) {
        secondaries = data.params["replicas"];
    }

    // Break up shard string into left and right
    size_t split = shard_str.find("-");
    if (shard_str.find("-inf") == 0) {
        split = shard_str.find("-", 1);
    }

    if (split == std::string::npos) {
        throw admin_parse_exc_t("incorrect shard specifier format");
    }

    shard_input_t shard_in;
    if (split != 0) {
        shard_in.left.exists = true;
        if (!cli_str_to_key(shard_str.substr(0, split), &shard_in.left.key)) {
            throw admin_parse_exc_t("could not parse key: " + shard_str.substr(0, split));
        }
    } else {
        shard_in.left.exists = false;
    }

    shard_in.right.unbounded = false;
    if (split < shard_str.length() - 1) {
        shard_in.right.exists = true;
        if (shard_str.substr(split + 1) == "+inf") {
            shard_in.right.unbounded = true;
        } else if (!cli_str_to_key(shard_str.substr(split + 1), &shard_in.right.key)) {
            throw admin_parse_exc_t("could not parse key: " + shard_str.substr(split + 1));
        }
    } else {
        shard_in.right.exists = false;
    }

    if (ns_path[0] == "memcached_namespaces") {
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator i = cluster_metadata.memcached_namespaces.namespaces.find(str_to_uuid(ns_path[1]));
        if (i == cluster_metadata.memcached_namespaces.namespaces.end() || i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error, could not find namespace: " + ns);
        }

        // If no primaries or secondaries are given, we list the current machine assignments
        if (primary.empty() && secondaries.empty()) {
            list_pinnings(i->second.get_mutable(), shard_in, cluster_metadata);
        } else {
            do_admin_pin_shard_internal(i->second.get_mutable(), shard_in, primary, secondaries, cluster_metadata);
        }
    } else {
        throw admin_cluster_exc_t("unexpected error, unknown namespace protocol");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

template <class protocol_t>
typename protocol_t::region_t admin_cluster_link_t::find_shard_in_namespace(namespace_semilattice_metadata_t<protocol_t>& ns,
                                                                            const shard_input_t& shard_in) {
    typename protocol_t::region_t shard;
    typename std::set<typename protocol_t::region_t>::iterator s;
    for (s = ns.shards.get_mutable().begin(); s != ns.shards.get_mutable().end(); ++s) {
        // TODO: This is a low level assertion.
        guarantee(s->beg == 0 && s->end == HASH_REGION_HASH_SIZE);

        if (shard_in.left.exists && s->inner.left != shard_in.left.key) {
            continue;
        } else if (shard_in.right.exists && (shard_in.right.unbounded != s->inner.right.unbounded)) {
            continue;
        } else if (shard_in.right.exists && !shard_in.right.unbounded && (shard_in.right.key != s->inner.right.key)) {
            continue;
        } else {
            shard = *s;
            break;
        }
    }

    if (s == ns.shards.get_mutable().end()) {
        throw admin_cluster_exc_t("could not find specified shard");
    }

    return shard;
}

template <class protocol_t>
void admin_cluster_link_t::do_admin_pin_shard_internal(namespace_semilattice_metadata_t<protocol_t>& ns,
                                                       const shard_input_t& shard_in,
                                                       const std::string& primary_str,
                                                       const std::vector<std::string>& secondary_strs,
                                                       cluster_semilattice_metadata_t& cluster_metadata) {
    machine_id_t primary(nil_uuid());
    std::multimap<datacenter_id_t, machine_id_t> datacenter_use;
    std::multimap<datacenter_id_t, machine_id_t> old_datacenter_use;
    typename region_map_t<protocol_t, machine_id_t>::iterator primary_shard;
    typename region_map_t<protocol_t, std::set<machine_id_t> >::iterator secondaries_shard;
    std::set<machine_id_t> secondaries;
    bool set_primary(!primary_str.empty());
    bool set_secondary(!secondary_strs.empty());

    // Check that none of the required fields are in conflict
    if (ns.shards.in_conflict()) {
        throw admin_cluster_exc_t("namespace shards are in conflict, run 'help resolve' for more information");
    } else if (ns.primary_pinnings.in_conflict()) {
        throw admin_cluster_exc_t("namespace primary pinnings are in conflict, run 'help resolve' for more information");
    } else if (ns.secondary_pinnings.in_conflict()) {
        throw admin_cluster_exc_t("namespace secondary pinnings are in conflict, run 'help resolve' for more information");
    } else if (ns.replica_affinities.in_conflict()) {
        throw admin_cluster_exc_t("namespace replica affinities are in conflict, run 'help resolve' for more information");
    } else if (ns.primary_datacenter.in_conflict()) {
        throw admin_cluster_exc_t("namespace primary datacenter is in conflict, run 'help resolve' for more information");
    }

    // Verify that the selected shard exists, and convert it into a region_t
    typename protocol_t::region_t shard = find_shard_in_namespace(ns, shard_in);

    // TODO: low-level hash_region_t shard assertion
    guarantee(shard.beg == 0 && shard.end == HASH_REGION_HASH_SIZE);

    // Verify primary is a valid machine and matches the primary datacenter
    if (set_primary) {
        std::vector<std::string> primary_path(get_info_from_id(primary_str)->path);
        primary = str_to_uuid(primary_path[1]);
        if (primary_path[0] != "machines") {
            throw admin_parse_exc_t("object is not a machine: " + primary_str);
        } else if (get_machine_datacenter(primary_str, str_to_uuid(primary_path[1]), cluster_metadata) != ns.primary_datacenter.get()) {
            throw admin_parse_exc_t("machine " + primary_str + " does not belong to the primary datacenter");
        }
    }

    // Verify secondaries are valid machines and store by datacenter for later
    for (size_t i = 0; i < secondary_strs.size(); ++i) {
        std::vector<std::string> secondary_path(get_info_from_id(secondary_strs[i])->path);
        machine_id_t machine = str_to_uuid(secondary_path[1]);

        if (set_primary && primary == machine) {
            throw admin_parse_exc_t("the same machine was specified as both a master and a replica: " + secondary_strs[i]);
        } else if (secondary_path[0] != "machines") {
            throw admin_parse_exc_t("object is not a machine: " + secondary_strs[i]);
        }

        datacenter_id_t datacenter = get_machine_datacenter(secondary_strs[i], machine, cluster_metadata);
        if (ns.replica_affinities.get().count(datacenter) == 0) {
            throw admin_parse_exc_t("machine " + secondary_strs[i] + " belongs to a datacenter with no affinity to namespace");
        }

        datacenter_use.insert(std::make_pair(datacenter, machine));
    }

    // Find the secondary pinnings and build the old datacenter pinning map if it exists
    for (secondaries_shard = ns.secondary_pinnings.get_mutable().begin();
         secondaries_shard != ns.secondary_pinnings.get_mutable().end(); ++secondaries_shard) {
        // TODO: low level hash_region_t assertion
        guarantee(secondaries_shard->first.beg == 0 && secondaries_shard->first.end == HASH_REGION_HASH_SIZE);

        if (secondaries_shard->first.inner.contains_key(shard.inner.left)) {
            break;
        }
    }

    if (secondaries_shard != ns.secondary_pinnings.get_mutable().end()) {
        for (std::set<machine_id_t>::iterator i = secondaries_shard->second.begin(); i != secondaries_shard->second.end(); ++i) {
            old_datacenter_use.insert(std::make_pair(get_machine_datacenter(uuid_to_str(*i), *i, cluster_metadata), *i));
        }
    }

    // Build the full set of secondaries, carry over any datacenters that were ignored in the command
    std::map<datacenter_id_t, int> affinities = ns.replica_affinities.get();
    for (std::map<datacenter_id_t, int>::iterator i = affinities.begin(); i != affinities.end(); ++i) {
        if (datacenter_use.count(i->first) == 0) {
            // No machines specified for this datacenter, copy over any from the old stuff
            for (std::multimap<datacenter_id_t, machine_id_t>::iterator j = old_datacenter_use.lower_bound(i->first); j != old_datacenter_use.end() && j->first == i->first; ++j) {
                // Filter out the new primary (if it exists)
                if (j->second == primary) {
                    set_secondary = true;
                } else {
                    secondaries.insert(j->second);
                }
            }
        } else if ((int)datacenter_use.count(i->first) <= i->second) {
            // Copy over all the specified machines for this datacenter
            for (std::multimap<datacenter_id_t, machine_id_t>::iterator j = datacenter_use.lower_bound(i->first);
                 j != datacenter_use.end() && j->first == i->first; ++j) {
                secondaries.insert(j->second);
            }
        } else {
            throw admin_cluster_exc_t("too many replicas requested from datacenter: " + uuid_to_str(i->first));
        }
    }

    // If we are not setting the primary, but the secondaries contain the existing primary, we have to clear the primary pinning
    if (!set_primary && set_secondary) {
        for (typename region_map_t<protocol_t, machine_id_t>::iterator primary_shard = ns.primary_pinnings.get_mutable().begin();
            primary_shard != ns.primary_pinnings.get_mutable().end(); ++primary_shard) {

            // TODO: Low level assertion
            guarantee(primary_shard->first.beg == 0 && primary_shard->first.end == HASH_REGION_HASH_SIZE);

            if (primary_shard->first.inner.contains_key(shard.inner.left)) {
                if (secondaries.count(primary_shard->second) != 0)
                    set_primary = true;
                break;
            }
        }
    }

    // Set primary and secondaries - do this before posting any changes in case anything goes wrong
    if (set_primary) {
        insert_pinning(ns.primary_pinnings.get_mutable(), shard.inner, primary);
        ns.primary_pinnings.upgrade_version(change_request_id);
    }
    if (set_secondary) {
        insert_pinning(ns.secondary_pinnings.get_mutable(), shard.inner, secondaries);
        ns.primary_pinnings.upgrade_version(change_request_id);
    }
}

// TODO: WTF are these template parameters.
template <class map_type, class value_type>
void insert_pinning(map_type& region_map, const key_range_t& shard, value_type& value) {
    map_type new_map;
    bool shard_done = false;

    for (typename map_type::iterator i = region_map.begin(); i != region_map.end(); ++i) {
        // TODO: low level hash_region_t assertion.
        guarantee(i->first.beg == 0 && i->first.end == HASH_REGION_HASH_SIZE);
        if (i->first.inner.contains_key(shard.left)) {
            if (i->first.inner.left != shard.left) {
                key_range_t new_shard(key_range_t::closed, i->first.inner.left, key_range_t::open, shard.left);
                new_map.set(hash_region_t<key_range_t>(new_shard), i->second);
            }
            new_map.set(hash_region_t<key_range_t>(shard), value);
            if (i->first.inner.right.key != shard.right.key) {
                // TODO: what if the shard we're looking for staggers the right bound
                key_range_t new_shard;
                new_shard.left = shard.right.key;
                new_shard.right = i->first.inner.right;
                new_map.set(hash_region_t<key_range_t>(new_shard), i->second);
            }
            shard_done = true;
        } else {
            // just copy over the shard
            new_map.set(i->first, i->second);
        }
    }

    if (!shard_done) {
        throw admin_cluster_exc_t("unexpected error, did not find the specified shard");
    }

    region_map = new_map;
}

// TODO: templatize on protocol
void admin_cluster_link_t::do_admin_split_shard(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::vector<std::string> ns_path(get_info_from_id(data.params["namespace"][0])->path);
    std::vector<std::string> split_points(data.params["split-points"]);
    std::string error;

    if (ns_path[0] == "memcached_namespaces") {
        namespace_id_t ns_id(str_to_uuid(ns_path[1]));
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator ns_it =
            cluster_metadata.memcached_namespaces.namespaces.find(ns_id);

        if (ns_it == cluster_metadata.memcached_namespaces.namespaces.end() || ns_it->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error when looking up namespace: " + ns_path[1]);
        }

        namespace_semilattice_metadata_t<memcached_protocol_t>& ns = ns_it->second.get_mutable();

        if (ns.shards.in_conflict()) {
            throw admin_cluster_exc_t("namespace shards are in conflict, run 'help resolve' for more information");
        }

        for (size_t i = 0; i < split_points.size(); ++i) {
            try {
                store_key_t key;
                if (!cli_str_to_key(split_points[i], &key)) {
                    throw admin_cluster_exc_t("split point could not be parsed: " + split_points[i]);
                }

                if (ns.shards.get().empty()) {
                    // this should never happen, but try to handle it anyway
                    key_range_t left(key_range_t::none, store_key_t(), key_range_t::open, store_key_t(key));
                    key_range_t right(key_range_t::closed, store_key_t(key), key_range_t::none, store_key_t());
                    ns.shards.get_mutable().insert(hash_region_t<key_range_t>(left));
                    ns.shards.get_mutable().insert(hash_region_t<key_range_t>(right));
                } else {
                    // TODO: use a better search than linear
                    std::set< hash_region_t<key_range_t> >::iterator shard = ns.shards.get_mutable().begin();
                    while (true) {
                        // TODO: This assertion is too low-level, there should be a function for hash_region_t that computes the expression.
                        guarantee(shard->beg == 0 && shard->end == HASH_REGION_HASH_SIZE);

                        if (shard == ns.shards.get_mutable().end()) {
                            throw admin_cluster_exc_t("split point could not be placed: " + split_points[i]);
                        } else if (shard->inner.contains_key(key)) {
                            break;
                        }
                        ++shard;
                    }

                    // Don't split if this key is already the split point
                    if (shard->inner.left == key) {
                        throw admin_cluster_exc_t("split point already exists: " + split_points[i]);
                    }

                    // Create the two new shards to be inserted
                    key_range_t left;
                    left.left = shard->inner.left;
                    left.right = key_range_t::right_bound_t(key);
                    key_range_t right;
                    right.left = key;
                    right.right = shard->inner.right;

                    ns.shards.get_mutable().erase(shard);
                    ns.shards.get_mutable().insert(hash_region_t<key_range_t>(left));
                    ns.shards.get_mutable().insert(hash_region_t<key_range_t>(right));
                }
                ns.shards.upgrade_version(change_request_id);
            } catch (std::exception& ex) {
                error += ex.what();
                error += "\n";
            }
        }

        // Any time shards are changed, we destroy existing pinnings
        // Use 'resolve' because they should be cleared even if in conflict
        // ID sent to the vector clock doesn't matter here since we're setting the metadata through HTTP (TODO: change this if that is no longer true)
        region_map_t<memcached_protocol_t, machine_id_t> new_primaries(memcached_protocol_t::region_t::universe(), nil_uuid());
        region_map_t<memcached_protocol_t, std::set<machine_id_t> > new_secondaries(memcached_protocol_t::region_t::universe(), std::set<machine_id_t>());

        ns.primary_pinnings = ns.primary_pinnings.make_resolving_version(new_primaries, change_request_id);
        ns.secondary_pinnings = ns.secondary_pinnings.make_resolving_version(new_secondaries, change_request_id);
    } else if (ns_path[0] == "dummy_namespaces") {
        throw admin_cluster_exc_t("splitting not supported for dummy namespaces");
    } else {
        throw admin_cluster_exc_t("invalid object type");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }

    if (!error.empty()) {
        if (split_points.size() > 1) {
            throw admin_cluster_exc_t(error + "not all split points were successfully added");
        } else {
            throw admin_cluster_exc_t(error.substr(0, error.length() - 1));
        }
    }
}

void admin_cluster_link_t::do_admin_merge_shard(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::vector<std::string> ns_path(get_info_from_id(data.params["namespace"][0])->path);
    std::vector<std::string> split_points(data.params["split-points"]);
    std::string error;

    if (ns_path[0] == "memcached_namespaces") {
        namespace_id_t ns_id(str_to_uuid(ns_path[1]));
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator ns_it =
            cluster_metadata.memcached_namespaces.namespaces.find(ns_id);

        if (ns_it == cluster_metadata.memcached_namespaces.namespaces.end() || ns_it->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error when looking up namespace: " + ns_path[1]);
        }

        namespace_semilattice_metadata_t<memcached_protocol_t>& ns = ns_it->second.get_mutable();

        if (ns.shards.in_conflict()) {
            throw admin_cluster_exc_t("namespace shards are in conflict, run 'help resolve' for more information");
        }

        for (size_t i = 0; i < split_points.size(); ++i) {
            try {
                store_key_t key;
                if (!cli_str_to_key(split_points[i], &key)) {
                    throw admin_cluster_exc_t("split point could not be parsed: " + split_points[i]);
                }

                // TODO: use a better search than linear
                std::set< hash_region_t<key_range_t> >::iterator shard = ns.shards.get_mutable().begin();

                if (shard == ns.shards.get_mutable().end()) {
                    throw admin_cluster_exc_t("split point does not exist: " + split_points[i]);
                }

                std::set< hash_region_t<key_range_t> >::iterator prev = shard;
                // TODO: This assertion's expression is too low-level, there should be a function for it.
                guarantee(shard->beg == 0 && shard->end == HASH_REGION_HASH_SIZE);

                ++shard;
                while (true) {
                    // TODO: This assertion's expression is too low-level, there should be a function for it.
                    guarantee(shard->beg == 0 && shard->end == HASH_REGION_HASH_SIZE);

                    if (shard == ns.shards.get_mutable().end()) {
                        throw admin_cluster_exc_t("split point does not exist: " + split_points[i]);
                    } else if (shard->inner.contains_key(key)) {
                        break;
                    }
                    prev = shard;
                    ++shard;
                }

                if (shard->inner.left != store_key_t(key)) {
                    throw admin_cluster_exc_t("split point does not exist: " + split_points[i]);
                }

                // Create the new shard to be inserted
                key_range_t merged;
                merged.left = prev->inner.left;
                merged.right = shard->inner.right;

                ns.shards.get_mutable().erase(shard);
                ns.shards.get_mutable().erase(prev);
                ns.shards.get_mutable().insert(hash_region_t<key_range_t>(merged));
                ns.shards.upgrade_version(change_request_id);
            } catch (std::exception& ex) {
                error += ex.what();
                error += "\n";
            }
        }

        // Any time shards are changed, we destroy existing pinnings
        // Use 'resolve' because they should be cleared even if in conflict
        // ID sent to the vector clock doesn't matter here since we're setting the metadata through HTTP (TODO: change this if that is no longer true)
        region_map_t<memcached_protocol_t, machine_id_t> new_primaries(memcached_protocol_t::region_t::universe(), nil_uuid());
        region_map_t<memcached_protocol_t, std::set<machine_id_t> > new_secondaries(memcached_protocol_t::region_t::universe(), std::set<machine_id_t>());

        ns.primary_pinnings = ns.primary_pinnings.make_resolving_version(new_primaries, change_request_id);
        ns.secondary_pinnings = ns.secondary_pinnings.make_resolving_version(new_secondaries, change_request_id);
    } else if (ns_path[0] == "dummy_namespaces") {
        throw admin_cluster_exc_t("merging not supported for dummy namespaces");
    } else {
        throw admin_cluster_exc_t("invalid object type");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }

    if (!error.empty()) {
        if (split_points.size() > 1) {
            throw admin_cluster_exc_t(error + "not all split points were successfully removed");
        } else {
            throw admin_cluster_exc_t(error.substr(0, error.length() - 1));
        }
    }
}

void admin_cluster_link_t::do_admin_list(admin_command_parser_t::command_data& data) {
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    std::string obj_str = (data.params.count("object") == 0 ? "" : data.params["object"][0]);
    bool long_format = (data.params.count("long") == 1);

    if (obj_str.empty()) {
        list_all(long_format, cluster_metadata);
    } else {
        metadata_info_t *info = get_info_from_id(obj_str);
        uuid_t obj_id = info->uuid;
        if (info->path[0] == "datacenters") {
            datacenters_semilattice_metadata_t::datacenter_map_t::iterator i = cluster_metadata.datacenters.datacenters.find(obj_id);
            if (i == cluster_metadata.datacenters.datacenters.end() || i->second.is_deleted()) {
                throw admin_cluster_exc_t("object not found: " + obj_str);
            }
            list_single_datacenter(obj_id, i->second.get_mutable(), cluster_metadata);
        } else if (info->path[0] == "dummy_namespaces") {
            namespaces_semilattice_metadata_t<mock::dummy_protocol_t>::namespace_map_t::iterator i = cluster_metadata.dummy_namespaces.namespaces.find(obj_id);
            if (i == cluster_metadata.dummy_namespaces.namespaces.end() || i->second.is_deleted()) {
                throw admin_cluster_exc_t("object not found: " + obj_str);
            }
            list_single_namespace(obj_id, i->second.get_mutable(), cluster_metadata, "dummy");
        } else if (info->path[0] == "memcached_namespaces") {
            namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator i = cluster_metadata.memcached_namespaces.namespaces.find(obj_id);
            if (i == cluster_metadata.memcached_namespaces.namespaces.end() || i->second.is_deleted()) {
                throw admin_cluster_exc_t("object not found: " + obj_str);
            }
            list_single_namespace(obj_id, i->second.get_mutable(), cluster_metadata, "memcached");
        } else if (info->path[0] == "machines") {
            machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.find(obj_id);
            if (i == cluster_metadata.machines.machines.end() || i->second.is_deleted()) {
                throw admin_cluster_exc_t("object not found: " + obj_str);
            }
            list_single_machine(obj_id, i->second.get_mutable(), cluster_metadata);
        } else {
            throw admin_cluster_exc_t("unexpected error, object found, but type not recognized: " + info->path[0]);
        }
    }
}

template <class protocol_t>
void admin_cluster_link_t::list_pinnings(namespace_semilattice_metadata_t<protocol_t>& ns, const shard_input_t& shard_in, cluster_semilattice_metadata_t& cluster_metadata) {
    if (ns.blueprint.in_conflict()) {
        throw admin_cluster_exc_t("namespace blueprint is in conflict");
    } else if (ns.shards.in_conflict()) {
        throw admin_cluster_exc_t("namespace shards are in conflict");
    }

    // Search through for the shard
    typename protocol_t::region_t shard = find_shard_in_namespace(ns, shard_in);

    // TODO: this is a low-level assertion.
    guarantee(shard.beg == 0 && shard.end == HASH_REGION_HASH_SIZE);

    list_pinnings_internal(ns.blueprint.get(), shard.inner, cluster_metadata);
}

template <class bp_type>
void admin_cluster_link_t::list_pinnings_internal(const bp_type& bp,
                                                  const key_range_t& shard,
                                                  cluster_semilattice_metadata_t& cluster_metadata) {
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;

    delta.push_back("type");
    delta.push_back("machine");
    delta.push_back("name");
    delta.push_back("datacenter");
    delta.push_back("name");
    table.push_back(delta);

    for (typename bp_type::role_map_t::const_iterator i = bp.machines_roles.begin(); i != bp.machines_roles.end(); ++i) {
        typename bp_type::region_to_role_map_t::const_iterator j = i->second.find(hash_region_t<key_range_t>(shard));
        if (j != i->second.end() && j->second != blueprint_details::role_nothing) {
            delta.clear();

            if (j->second == blueprint_details::role_primary) {
                delta.push_back("master");
            } else if (j->second == blueprint_details::role_secondary) {
                delta.push_back("replica");
            } else {
                throw admin_cluster_exc_t("unexpected error, unrecognized role type encountered");
            }

            delta.push_back(truncate_uuid(i->first));

            // Find the machine to get the datacenter and name
            machines_semilattice_metadata_t::machine_map_t::iterator m = cluster_metadata.machines.machines.find(i->first);
            if (m == cluster_metadata.machines.machines.end() || m->second.is_deleted()) {
                throw admin_cluster_exc_t("unexpected error, blueprint invalid");
            }

            if (m->second.get().name.in_conflict()) {
                delta.push_back("<conflict>");
            } else {
                delta.push_back(m->second.get().name.get());
            }

            if (m->second.get().datacenter.in_conflict()) {
                delta.push_back("<conflict>");
                delta.push_back("");
            } else {
                delta.push_back(truncate_uuid(m->second.get().datacenter.get()));

                // Find the datacenter to get the name
                datacenters_semilattice_metadata_t::datacenter_map_t::iterator dc = cluster_metadata.datacenters.datacenters.find(m->second.get().datacenter.get());
                if (dc == cluster_metadata.datacenters.datacenters.end() || dc->second.is_deleted()) {
                    throw admin_cluster_exc_t("unexpected error, blueprint invalid");
                }

                if (dc->second.get().name.in_conflict()) {
                    delta.push_back("<conflict>");
                } else {
                    delta.push_back(dc->second.get().name.get());
                }
            }

            table.push_back(delta);
        }
    }

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

struct admin_stats_request_t {
    explicit admin_stats_request_t(mailbox_manager_t *mailbox_manager) :
        response_mailbox(mailbox_manager,
                         boost::bind(&promise_t<perfmon_result_t>::pulse, &stats_promise, _1),
                         mailbox_callback_mode_inline) { }
    promise_t<perfmon_result_t> stats_promise;
    mailbox_t<void(perfmon_result_t)> response_mailbox;
};

void admin_cluster_link_t::do_admin_list_stats(admin_command_parser_t::command_data& data) {
    std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    boost::ptr_map<machine_id_t, admin_stats_request_t> request_map;
    std::set<machine_id_t> machine_filters;
    std::set<namespace_id_t> namespace_filters;
    std::string stat_filter;
    signal_timer_t timer(5000); // 5 second timeout to get all stats

    // Check command params for namespace or machine filter
    if (data.params.count("id-filter") == 1) {
        for (size_t i = 0; i < data.params["id-filter"].size(); ++i) {
            std::string temp = data.params["id-filter"][i];
            metadata_info_t *info = get_info_from_id(temp);
            if (info->path[0] == "machines") {
                machine_filters.insert(info->uuid);
            } else if (info->path[0] == "dummy_namespaces" || info->path[0] == "memcached_namespaces") {
                namespace_filters.insert(info->uuid);
            } else {
                throw admin_parse_exc_t("object filter is not a machine or namespace: " + temp);
            }
        }
    }

    // Get the set of machines to request stats from and construct mailboxes for the responses
    if (machine_filters.empty()) {
        for (machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.begin();
             i != cluster_metadata.machines.machines.end(); ++i) {
            if (!i->second.is_deleted()) {
                machine_id_t target = i->first;
                request_map.insert(target, new admin_stats_request_t(&mailbox_manager));
            }
        }
    } else {
        for (std::set<machine_id_t>::iterator i = machine_filters.begin(); i != machine_filters.end(); ++i) {
            machine_id_t id = *i;
            request_map.insert(id, new admin_stats_request_t(&mailbox_manager));
        }
    }

    if (request_map.empty()) {
        throw admin_cluster_exc_t("no machines to query stats from");
    }

    // Send the requests
    for (boost::ptr_map<machine_id_t, admin_stats_request_t>::iterator i = request_map.begin(); i != request_map.end(); ++i) {
        bool found = false;
        // Find machine in directory
        // TODO: do a better than linear search
        for (std::map<peer_id_t, cluster_directory_metadata_t>::iterator j = directory.begin(); j != directory.end(); ++j) {
            if (j->second.machine_id == i->first) {
                found = true;
                send(&mailbox_manager,
                     j->second.get_stats_mailbox_address,
                     i->second->response_mailbox.get_address(),
                     std::set<stat_manager_t::stat_id_t>());
            }
        }

        if (!found) {
            throw admin_cluster_exc_t("Could not locate machine in directory: " + uuid_to_str(i->first));
        }
    }

    std::vector<std::vector<std::string> > stats_table;
    std::vector<std::string> header;

    header.push_back("machine");
    header.push_back("category");
    header.push_back("stat");
    header.push_back("value");
    stats_table.push_back(header);

    // Wait for responses and output them, filtering as necessary
    for (boost::ptr_map<machine_id_t, admin_stats_request_t>::iterator i = request_map.begin(); i != request_map.end(); ++i) {
        signal_t *stats_ready = i->second->stats_promise.get_ready_signal();
        wait_any_t waiter(&timer, stats_ready);
        waiter.wait();

        if (stats_ready->is_pulsed()) {
            perfmon_result_t stats = i->second->stats_promise.wait();
            std::string machine_name = get_info_from_id(uuid_to_str(i->first))->name;

            // If namespaces were selected, only list stats belonging to those namespaces
            if (!namespace_filters.empty()) {
                for (perfmon_result_t::const_iterator i = stats.begin(); i != stats.end(); ++i) {
                    if (is_uuid(i->first) && namespace_filters.count(str_to_uuid(i->first)) == 1) {
                        // Try to convert the uuid to a (unique) name
                        std::string id = i->first;
                        try {
                            uuid_t temp = str_to_uuid(id);
                            id = get_info_from_id(uuid_to_str(temp))->name;
                        } catch (...) {
                        }
                        admin_stats_to_table(machine_name, id, *i->second, stats_table);
                    }
                }
            } else {
                admin_stats_to_table(machine_name, std::string(), stats, stats_table);
            }
        }
    }

    if (stats_table.size() > 1) {
        admin_print_table(stats_table);
    }
}

void admin_cluster_link_t::do_admin_list_directory(admin_command_parser_t::command_data& data) {
    std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    bool long_format = data.params.count("long");
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;

    delta.push_back("type");
    delta.push_back("name");
    delta.push_back("uuid");
    delta.push_back("ips");
    table.push_back(delta);

    for (std::map<peer_id_t, cluster_directory_metadata_t>::iterator i = directory.begin(); i != directory.end(); i++) {
        delta.clear();

        switch (i->second.peer_type) {
          case ADMIN_PEER: delta.push_back("admin"); break;
          case SERVER_PEER: delta.push_back("server"); break;
          case PROXY_PEER: delta.push_back("proxy"); break;
          default: unreachable();
        }

        machines_semilattice_metadata_t::machine_map_t::iterator m = cluster_metadata.machines.machines.find(i->second.machine_id);
        if (m != cluster_metadata.machines.machines.end()) {
            if (m->second.is_deleted()) {
                delta.push_back("<deleted>");
            } else if (m->second.get().name.in_conflict()) {
                delta.push_back("<conflict>");
            } else {
                delta.push_back(m->second.get().name.get());
            }
        } else {
            delta.push_back("");
        }

        if (long_format) {
            delta.push_back(uuid_to_str(i->second.machine_id));
        } else {
            delta.push_back(uuid_to_str(i->second.machine_id).substr(0, uuid_output_length));
        }

        std::string ips;
        for (size_t j = 0; j != i->second.ips.size(); ++j) {
            ips += (j == 0 ? "" : " ") + i->second.ips[j];
        }
        delta.push_back(ips);

        table.push_back(delta);
    }

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

void admin_cluster_link_t::do_admin_list_issues(admin_command_parser_t::command_data& data UNUSED) {
    std::list<clone_ptr_t<global_issue_t> > issues = admin_tracker.issue_aggregator.get_issues();
    for (std::list<clone_ptr_t<global_issue_t> >::iterator i = issues.begin(); i != issues.end(); ++i) {
        puts((*i)->get_description().c_str());
    }
}

template <class map_type>
void admin_cluster_link_t::list_all_internal(const std::string& type, bool long_format, map_type& obj_map, std::vector<std::vector<std::string> >& table) {
    std::vector<std::string> delta;
    for (typename map_type::iterator i = obj_map.begin(); i != obj_map.end(); ++i) {
        if (!i->second.is_deleted()) {
            delta.clear();

            delta.push_back(type);

            if (long_format) {
                delta.push_back(uuid_to_str(i->first));
            } else {
                delta.push_back(truncate_uuid(i->first));
            }

            if (i->second.get().name.in_conflict()) {
                delta.push_back("<conflict>");
            } else {
                delta.push_back(i->second.get().name.get());
            }

            table.push_back(delta);
        }
    }
}

void admin_cluster_link_t::list_all(bool long_format, cluster_semilattice_metadata_t& cluster_metadata) {
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;

    delta.push_back("type");
    delta.push_back("uuid");
    delta.push_back("name");
    table.push_back(delta);

    list_all_internal("machine", long_format, cluster_metadata.machines.machines, table);
    list_all_internal("datacenter", long_format, cluster_metadata.datacenters.datacenters, table);
    // TODO: better differentiation between namespace types
    list_all_internal("namespace (d)", long_format, cluster_metadata.dummy_namespaces.namespaces, table);
    list_all_internal("namespace (m)", long_format, cluster_metadata.memcached_namespaces.namespaces, table);

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

std::map<datacenter_id_t, admin_cluster_link_t::datacenter_info_t> admin_cluster_link_t::build_datacenter_info(cluster_semilattice_metadata_t& cluster_metadata) {
    std::map<datacenter_id_t, datacenter_info_t> results;
    std::map<machine_id_t, machine_info_t> machine_data = build_machine_info(cluster_metadata);

    for (machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted() && !i->second.get().datacenter.in_conflict()) {
            datacenter_id_t datacenter = i->second.get().datacenter.get();

            results[datacenter].machines += 1;

            std::map<machine_id_t, machine_info_t>::iterator info = machine_data.find(i->first);
            if (info != machine_data.end()) {
                results[datacenter].primaries += info->second.primaries;
                results[datacenter].secondaries += info->second.secondaries;
            }
        }
    }

    // TODO: this will list affinities, but not actual state (in case of impossible requirements)
    add_datacenter_affinities(cluster_metadata.dummy_namespaces.namespaces, results);
    add_datacenter_affinities(cluster_metadata.memcached_namespaces.namespaces, results);

    return results;
}

template <class map_type>
void admin_cluster_link_t::add_datacenter_affinities(const map_type& ns_map, std::map<datacenter_id_t, datacenter_info_t>& results) {
    for (typename map_type::const_iterator i = ns_map.begin(); i != ns_map.end(); ++i) {
        if (!i->second.is_deleted()) {
            if (!i->second.get().primary_datacenter.in_conflict()) {
                ++results[i->second.get().primary_datacenter.get()].namespaces;
            }

            if (!i->second.get().replica_affinities.in_conflict()) {
                std::map<datacenter_id_t, int> affinities = i->second.get().replica_affinities.get();
                for (std::map<datacenter_id_t, int>::iterator j = affinities.begin(); j != affinities.end(); ++j) {
                    if (j->second > 0) {
                        ++results[j->first].namespaces;
                    }
                }
            }
        }
    }
}

void admin_cluster_link_t::do_admin_list_datacenters(admin_command_parser_t::command_data& data) {
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    bool long_format = (data.params.count("long") == 1);

    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;
    std::map<datacenter_id_t, datacenter_info_t> long_info;

    delta.push_back("uuid");
    delta.push_back("name");
    if (long_format) {
        delta.push_back("machines");
        delta.push_back("namespaces");
        delta.push_back("primaries");
        delta.push_back("secondaries");
    }

    table.push_back(delta);

    if (long_format) {
        long_info = build_datacenter_info(cluster_metadata);
    }

    for (datacenters_semilattice_metadata_t::datacenter_map_t::const_iterator i = cluster_metadata.datacenters.datacenters.begin(); i != cluster_metadata.datacenters.datacenters.end(); ++i) {
        if (!i->second.is_deleted()) {
            delta.clear();

            if (long_format) {
                delta.push_back(uuid_to_str(i->first));
            } else {
                delta.push_back(truncate_uuid(i->first));
            }

            if (i->second.get().name.in_conflict()) {
                delta.push_back("<conflict>");
            } else {
                delta.push_back(i->second.get().name.get());
            }

            if (long_format) {
                char buffer[64];
                datacenter_info_t info = long_info[i->first];
                snprintf(buffer, sizeof(buffer), "%ld", info.machines);
                delta.push_back(buffer);
                snprintf(buffer, sizeof(buffer), "%ld", info.namespaces);
                delta.push_back(buffer);
                snprintf(buffer, sizeof(buffer), "%ld", info.primaries);
                delta.push_back(buffer);
                snprintf(buffer, sizeof(buffer), "%ld", info.secondaries);
                delta.push_back(buffer);
            }
            table.push_back(delta);
        }
    }

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

template <class ns_type>
admin_cluster_link_t::namespace_info_t admin_cluster_link_t::get_namespace_info(ns_type& ns) {
    namespace_info_t result;

    if (ns.shards.in_conflict()) {
        result.shards = -1;
    } else {
        result.shards = ns.shards.get().size();
    }

    // For replicas, go through the blueprint and sum up all roles
    if (ns.blueprint.in_conflict()) {
        result.replicas = -1;
    } else {
        result.replicas = get_replica_count_from_blueprint(ns.blueprint.get_mutable());
    }

    if (ns.primary_datacenter.in_conflict()) {
        result.primary.assign("<conflict>");
    } else {
        result.primary.assign(uuid_to_str(ns.primary_datacenter.get()));
    }

    return result;
}

template <class bp_type>
size_t admin_cluster_link_t::get_replica_count_from_blueprint(const bp_type& bp) {
    size_t count = 0;

    for (typename bp_type::role_map_t::const_iterator j = bp.machines_roles.begin();
         j != bp.machines_roles.end(); ++j) {
        for (typename bp_type::region_to_role_map_t::const_iterator k = j->second.begin();
             k != j->second.end(); ++k) {
            if (k->second == blueprint_details::role_primary) {
                ++count;
            } else if (k->second == blueprint_details::role_secondary) {
                ++count;
            }
        }
    }
    return count;
}

void admin_cluster_link_t::do_admin_list_namespaces(admin_command_parser_t::command_data& data) {
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    std::string type = (data.params.count("protocol") == 0 ? "" : data.params["protocol"][0]);
    bool long_format = (data.params.count("long") == 1);

    std::vector<std::vector<std::string> > table;
    std::vector<std::string> header;

    header.push_back("uuid");
    header.push_back("name");
    header.push_back("protocol");
    if (long_format) {
        header.push_back("shards");
        header.push_back("replicas");
        header.push_back("primary");
    }

    table.push_back(header);

    if (type.empty()) {
        add_namespaces("dummy", long_format, cluster_metadata.dummy_namespaces.namespaces, table);
        add_namespaces("memcached", long_format, cluster_metadata.memcached_namespaces.namespaces, table);
    } else if (type == "dummy") {
        add_namespaces(type, long_format, cluster_metadata.dummy_namespaces.namespaces, table);
    } else if (type == "memcached") {
        add_namespaces(type, long_format, cluster_metadata.memcached_namespaces.namespaces, table);
    } else {
        throw admin_parse_exc_t("unrecognized namespace type: " + type);
    }

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

template <class map_type>
void admin_cluster_link_t::add_namespaces(const std::string& protocol, bool long_format, map_type& namespaces, std::vector<std::vector<std::string> >& table) {

    std::vector<std::string> delta;
    for (typename map_type::iterator i = namespaces.begin(); i != namespaces.end(); ++i) {
        if (!i->second.is_deleted()) {
            delta.clear();

            if (long_format) {
                delta.push_back(uuid_to_str(i->first));
            } else {
                delta.push_back(truncate_uuid(i->first));
            }

            if (!i->second.get().name.in_conflict()) {
                delta.push_back(i->second.get().name.get());
            } else {
                delta.push_back("<conflict>");
            }

            delta.push_back(protocol);

            if (long_format) {
                char buffer[64];
                namespace_info_t info = get_namespace_info(i->second.get_mutable());

                if (info.shards != -1) {
                    snprintf(buffer, sizeof(buffer), "%i", info.shards);
                    delta.push_back(buffer);
                } else {
                    delta.push_back("<conflict>");
                }

                if (info.replicas != -1) {
                    snprintf(buffer, sizeof(buffer), "%i", info.replicas);
                    delta.push_back(buffer);
                } else {
                    delta.push_back("<conflict>");
                }

                delta.push_back(info.primary);
            }

            table.push_back(delta);
        }
    }
}

std::map<machine_id_t, admin_cluster_link_t::machine_info_t> admin_cluster_link_t::build_machine_info(cluster_semilattice_metadata_t& cluster_metadata) {
    std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();
    std::map<machine_id_t, machine_info_t> results;

    // Initialize each machine, reachable machines are in the directory
    for (std::map<peer_id_t, cluster_directory_metadata_t>::iterator i = directory.begin(); i != directory.end(); ++i) {
        results.insert(std::make_pair(i->second.machine_id, machine_info_t()));
        results[i->second.machine_id].status.assign("reach");
    }

    // Unreachable machines will be found in the metadata but not the directory
    for (machines_semilattice_metadata_t::machine_map_t::const_iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted()) {
            if (results.count(i->first) == 0) {
                results.insert(std::make_pair(i->first, machine_info_t()));
                results[i->first].status.assign("unreach");
            }
        }
    }

    // Go through namespaces
    build_machine_info_internal(cluster_metadata.dummy_namespaces.namespaces, results);
    build_machine_info_internal(cluster_metadata.memcached_namespaces.namespaces, results);

    return results;
}

template <class map_type>
void admin_cluster_link_t::build_machine_info_internal(const map_type& ns_map, std::map<machine_id_t, machine_info_t>& results) {
    for (typename map_type::const_iterator i = ns_map.begin(); i != ns_map.end(); ++i) {
        if (!i->second.is_deleted() && !i->second.get().blueprint.in_conflict()) {
            add_machine_info_from_blueprint(i->second.get().blueprint.get_mutable(), results);
        }
    }
}

template <class bp_type>
void admin_cluster_link_t::add_machine_info_from_blueprint(const bp_type& bp, std::map<machine_id_t, machine_info_t>& results) {
    for (typename bp_type::role_map_t::const_iterator j = bp.machines_roles.begin();
         j != bp.machines_roles.end(); ++j) {
        if (results.count(j->first) == 0) {
            continue;
        }

        bool machine_used = false;

        for (typename bp_type::region_to_role_map_t::const_iterator k = j->second.begin();
             k != j->second.end(); ++k) {
            if (k->second == blueprint_details::role_primary) {
                ++results[j->first].primaries;
                machine_used = true;
            } else if (k->second == blueprint_details::role_secondary) {
                ++results[j->first].secondaries;
                machine_used = true;
            }
        }

        if (machine_used) {
            ++results[j->first].namespaces;
        }
    }
}

void admin_cluster_link_t::do_admin_list_machines(admin_command_parser_t::command_data& data) {
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    bool long_format = (data.params.count("long") == 1);

    std::map<machine_id_t, machine_info_t> long_info;
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;

    delta.push_back("uuid");
    delta.push_back("name");
    delta.push_back("datacenter");
    if (long_format) {
        delta.push_back("status");
        delta.push_back("namespaces");
        delta.push_back("primaries");
        delta.push_back("secondaries");
    }

    table.push_back(delta);

    if (long_format) {
        long_info = build_machine_info(cluster_metadata);
    }

    for (machines_semilattice_metadata_t::machine_map_t::const_iterator i = cluster_metadata.machines.machines.begin(); i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted()) {
            delta.clear();

            if (long_format) {
                delta.push_back(uuid_to_str(i->first));
            } else {
                delta.push_back(truncate_uuid(i->first));
            }

            if (!i->second.get().name.in_conflict()) {
                delta.push_back(i->second.get().name.get());
            } else {
                delta.push_back("<conflict>");
            }

            if (!i->second.get().datacenter.in_conflict()) {
                if (i->second.get().datacenter.get().is_nil()) {
                    delta.push_back("none");
                } else if (long_format) {
                    delta.push_back(uuid_to_str(i->second.get().datacenter.get()));
                } else {
                    delta.push_back(truncate_uuid(i->second.get().datacenter.get()));
                }
            } else {
                delta.push_back("<conflict>");
            }

            if (long_format) {
                char buffer[64];
                machine_info_t info = long_info[i->first];
                delta.push_back(info.status);
                snprintf(buffer, sizeof(buffer), "%ld", info.namespaces);
                delta.push_back(buffer);
                snprintf(buffer, sizeof(buffer), "%ld", info.primaries);
                delta.push_back(buffer);
                snprintf(buffer, sizeof(buffer), "%ld", info.secondaries);
                delta.push_back(buffer);
            }

            table.push_back(delta);
        }
    }

    // TODO: sort by datacenter and name

    if (table.size() > 1) {
        admin_print_table(table);
    }
}

void admin_cluster_link_t::do_admin_create_datacenter(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    datacenter_id_t new_id = generate_uuid();
    datacenter_semilattice_metadata_t& datacenter = cluster_metadata.datacenters.datacenters[new_id].get_mutable();

    datacenter.name.get_mutable() = data.params["name"][0];
    datacenter.name.upgrade_version(change_request_id);

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }

    printf("uuid: %s\n", uuid_to_str(new_id).c_str());
}

void admin_cluster_link_t::do_admin_create_namespace(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::string protocol(data.params["protocol"][0]);
    std::string port_str(data.params["port"][0]);
    std::string name(data.params["name"][0]);
    uint64_t port;
    std::string datacenter_id(data.params["primary"][0]);
    metadata_info_t *datacenter_info(get_info_from_id(datacenter_id));
    datacenter_id_t primary(str_to_uuid(datacenter_info->path[1]));
    namespace_id_t new_id;

    // Make sure port is a number
    if (!strtou64_strict(port_str, 10, &port)) {
        throw admin_parse_exc_t("port is not a number");
    }

    if (port > 65536) {
        throw admin_parse_exc_t("port is too large: " + port_str);
    }

    if (datacenter_info->path[0] != "datacenters") {
        throw admin_parse_exc_t("namespace primary is not a datacenter: " + datacenter_id);
    }

    // Verify that the datacenter has at least one machine in it
    if (get_machine_count_in_datacenter(cluster_metadata, datacenter_info->uuid) < 1) {
        throw admin_cluster_exc_t("primary datacenter must have at least one machine, run 'help set datacenter' for more information");
    }

    if (protocol == "memcached") {
        new_id = do_admin_create_namespace_internal(cluster_metadata.memcached_namespaces, name, port, primary);
    } else if (protocol == "dummy") {
        new_id = do_admin_create_namespace_internal(cluster_metadata.dummy_namespaces, name, port, primary);
    } else {
        throw admin_parse_exc_t("unrecognized protocol: " + protocol);
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
    printf("uuid: %s\n", uuid_to_str(new_id).c_str());
}

template <class protocol_t>
namespace_id_t admin_cluster_link_t::do_admin_create_namespace_internal(namespaces_semilattice_metadata_t<protocol_t>& ns,
                                                                        const std::string& name,
                                                                        int port,
                                                                        const datacenter_id_t& primary) {
    namespace_id_t id = generate_uuid();
    namespace_semilattice_metadata_t<protocol_t>& obj = ns.namespaces[id].get_mutable();

    obj.name.get_mutable() = name;
    obj.name.upgrade_version(change_request_id);

    if (!primary.is_nil()) {
        obj.primary_datacenter.get_mutable() = primary;
        obj.primary_datacenter.upgrade_version(change_request_id);
    }

    obj.port.get_mutable() = port;
    obj.port.upgrade_version(change_request_id);

    std::set<typename protocol_t::region_t> shards;
    shards.insert(protocol_t::region_t::universe());
    obj.shards.get_mutable() = shards;
    obj.shards.upgrade_version(change_request_id);

    return id;
}

void admin_cluster_link_t::do_admin_set_datacenter(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::string obj_id(data.params["id"][0]);
    metadata_info_t *obj_info(get_info_from_id(obj_id));
    std::string datacenter_id(data.params["datacenter"][0]);
    metadata_info_t *datacenter_info(get_info_from_id(datacenter_id));
    datacenter_id_t datacenter_uuid(datacenter_info->uuid);

    // Target must be a datacenter in all existing use cases
    if (datacenter_info->path[0] != "datacenters") {
        throw admin_parse_exc_t("destination is not a datacenter: " + datacenter_id);
    }

    if (obj_info->path[0] == "memcached_namespaces") {
        do_admin_set_datacenter_namespace(cluster_metadata.memcached_namespaces.namespaces, obj_info->uuid, datacenter_uuid);
    } else if (obj_info->path[0] == "dummy_namespaces") {
        do_admin_set_datacenter_namespace(cluster_metadata.memcached_namespaces.namespaces, obj_info->uuid, datacenter_uuid);
    } else if (obj_info->path[0] == "machines") {
        do_admin_set_datacenter_machine(cluster_metadata.machines.machines, obj_info->uuid, datacenter_uuid, cluster_metadata);
    } else {
        throw admin_cluster_exc_t("target object is not a namespace or machine");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

template <class obj_map>
void admin_cluster_link_t::do_admin_set_datacenter_namespace(obj_map& metadata,
                                                             const uuid_t obj_uuid,
                                                             const datacenter_id_t dc) {
    typename obj_map::iterator i = metadata.find(obj_uuid);
    if (i == metadata.end() || i->second.is_deleted()) {
        throw admin_cluster_exc_t("unexpected error when looking up object: " + uuid_to_str(obj_uuid));
    } else if (i->second.get_mutable().primary_datacenter.in_conflict()) {
        throw admin_cluster_exc_t("namespace's primary datacenter is in conflict, run 'help resolve' for more information");
    }

    i->second.get_mutable().primary_datacenter.get_mutable() = dc;
    i->second.get_mutable().primary_datacenter.upgrade_version(change_request_id);
}

void admin_cluster_link_t::do_admin_set_datacenter_machine(machines_semilattice_metadata_t::machine_map_t& metadata,
                                                           const uuid_t obj_uuid,
                                                           const datacenter_id_t dc,
                                                           cluster_semilattice_metadata_t& cluster_metadata) {
    machines_semilattice_metadata_t::machine_map_t::iterator i = metadata.find(obj_uuid);
    if (i == metadata.end() || i->second.is_deleted()) {
        throw admin_cluster_exc_t("unexpected error when looking up object: " + uuid_to_str(obj_uuid));
    } else if (i->second.get_mutable().datacenter.in_conflict()) {
        throw admin_cluster_exc_t("machine's datacenter is in conflict, run 'help resolve' for more information");
    }

    datacenter_id_t old_datacenter(i->second.get_mutable().datacenter.get());
    i->second.get_mutable().datacenter.get_mutable() = dc;
    i->second.get_mutable().datacenter.upgrade_version(change_request_id);

    // If the datacenter has changed (or we couldn't determine the old datacenter uuid), clear pinnings
    if (old_datacenter != dc) {
        remove_machine_pinnings(obj_uuid, cluster_metadata.memcached_namespaces.namespaces);
        remove_machine_pinnings(obj_uuid, cluster_metadata.dummy_namespaces.namespaces);
    }
}

template <class protocol_t>
void admin_cluster_link_t::remove_machine_pinnings(const machine_id_t& machine,
                                                   std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t<protocol_t> > >& ns_map) {
    // TODO: what if a pinning is in conflict with this machine specified, but is resolved later?
    // perhaps when a resolve is issued, check to make sure it is still valid

    for (typename namespaces_semilattice_metadata_t<protocol_t>::namespace_map_t::iterator i = ns_map.begin(); i != ns_map.end(); ++i) {
        if (i->second.is_deleted()) {
            continue;
        }

        namespace_semilattice_metadata_t<protocol_t>& ns = i->second.get_mutable();

        // Check for and remove the machine in primary pinnings
        if (!ns.primary_pinnings.in_conflict()) {
            bool do_upgrade = false;
            for (typename region_map_t<protocol_t, machine_id_t>::iterator j = ns.primary_pinnings.get_mutable().begin();
                 j != ns.primary_pinnings.get_mutable().end(); ++j) {
                if (j->second == machine) {
                    j->second = nil_uuid();
                    do_upgrade = true;
                }
            }

            if (do_upgrade) {
                ns.primary_pinnings.upgrade_version(change_request_id);
            }
        }

        // Check for and remove the machine in secondary pinnings
        if (!ns.secondary_pinnings.in_conflict()) {
            bool do_upgrade = false;
            for (typename region_map_t<protocol_t, std::set<machine_id_t> >::iterator j = ns.secondary_pinnings.get_mutable().begin();
                 j != ns.secondary_pinnings.get_mutable().end(); ++j) {
                if (j->second.erase(machine) == 1) {
                    do_upgrade = true;
                }
            }

            if (do_upgrade) {
                ns.secondary_pinnings.upgrade_version(change_request_id);
            }
        }
    }
}

void admin_cluster_link_t::do_admin_set_name(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    metadata_info_t *info = get_info_from_id(data.params["id"][0]);
    std::string name = data.params["new-name"][0];

    // TODO: make sure names aren't silly things like uuids or reserved strings

    if (info->path[0] == "machines") {
        do_admin_set_name_internal(cluster_metadata.machines.machines, info->uuid, name);
    } else if (info->path[0] == "datacenters") {
        do_admin_set_name_internal(cluster_metadata.datacenters.datacenters, info->uuid, name);
    } else if (info->path[0] == "dummy_namespaces") { 
        do_admin_set_name_internal(cluster_metadata.dummy_namespaces.namespaces, info->uuid, name);
    } else if (info->path[0] == "memcached_namespaces") {
        do_admin_set_name_internal(cluster_metadata.memcached_namespaces.namespaces, info->uuid, name);
    } else {
        throw admin_cluster_exc_t("unrecognized object type");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

template <class map_type>
void admin_cluster_link_t::do_admin_set_name_internal(map_type& obj_map, const uuid_t& id, const std::string& name) {
    typename map_type::iterator i = obj_map.find(id);
    if (!i->second.is_deleted() && !i->second.get().name.in_conflict()) {
        i->second.get_mutable().name.get_mutable() = name;
        i->second.get_mutable().name.upgrade_version(change_request_id);
    }
}

void admin_cluster_link_t::do_admin_set_acks(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    metadata_info_t *ns_info(get_info_from_id(data.params["namespace"][0]));
    metadata_info_t *dc_info(get_info_from_id(data.params["datacenter"][0]));
    std::string acks_str = data.params["num-acks"][0].c_str();
    uint64_t acks_num;

    // Make sure num-acks is a number
    if (!strtou64_strict(acks_str, 10, &acks_num)) {
        throw admin_parse_exc_t("num-acks is not a number");
    }

    if (dc_info->path[0] != "datacenters") {
        throw admin_parse_exc_t(data.params["datacenter"][0] + " is not a datacenter");
    }

    if (ns_info->path[0] == "dummy_namespaces") {
        namespaces_semilattice_metadata_t<mock::dummy_protocol_t>::namespace_map_t::iterator i = cluster_metadata.dummy_namespaces.namespaces.find(ns_info->uuid);
        if (i == cluster_metadata.dummy_namespaces.namespaces.end()) {
            throw admin_parse_exc_t("unexpected error, namespace not found");
        } else if (i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error, namespace has been deleted");
        }
        do_admin_set_acks_internal(i->second.get_mutable(), dc_info->uuid, acks_num);

    } else if (ns_info->path[0] == "memcached_namespaces") {
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator i = cluster_metadata.memcached_namespaces.namespaces.find(ns_info->uuid);
        if (i == cluster_metadata.memcached_namespaces.namespaces.end()) {
            throw admin_parse_exc_t("unexpected error, namespace not found");
        } else if (i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error, namespace has been deleted");
        }
        do_admin_set_acks_internal(i->second.get_mutable(), dc_info->uuid, acks_num);

    } else {
        throw admin_parse_exc_t(data.params["namespace"][0] + " is not a namespace");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

template <class protocol_t>
void admin_cluster_link_t::do_admin_set_acks_internal(namespace_semilattice_metadata_t<protocol_t>& ns, const datacenter_id_t& datacenter, int num_acks) {
    if (ns.primary_datacenter.in_conflict()) {
        throw admin_cluster_exc_t("namespace primary datacenter is in conflict, run 'help resolve' for more information");
    }

    if (ns.replica_affinities.in_conflict()) {
        throw admin_cluster_exc_t("namespace replica affinities are in conflict, run 'help resolve' for more information");
    }

    if (ns.ack_expectations.in_conflict()) {
        throw admin_cluster_exc_t("namespace ack expectations are in conflict, run 'help resolve' for more information");
    }

    // Make sure the selected datacenter is assigned to the namespace and that the number of replicas is less than or equal to the number of acks
    std::map<datacenter_id_t, int>::iterator i = ns.replica_affinities.get().find(datacenter);
    bool is_primary = (datacenter == ns.primary_datacenter.get());
    if ((i == ns.replica_affinities.get().end() || i->second == 0) && !is_primary) {
        throw admin_cluster_exc_t("the specified datacenter has no replica affinities with the given namespace");
    } else if (num_acks > i->second + (is_primary ? 1 : 0)) {
        throw admin_cluster_exc_t("cannot assign more ack expectations than replicas in a datacenter");
    }

    ns.ack_expectations.get_mutable()[datacenter] = num_acks;
    ns.ack_expectations.upgrade_version(change_request_id);
}

void admin_cluster_link_t::do_admin_set_replicas(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    metadata_info_t *ns_info(get_info_from_id(data.params["namespace"][0]));
    metadata_info_t *dc_info(get_info_from_id(data.params["datacenter"][0]));
    std::string replicas_str = data.params["num-replicas"][0].c_str();
    uint64_t num_replicas;

    // Make sure num-replicas is a number
    if (!strtou64_strict(replicas_str, 10, &num_replicas)) {
        throw admin_parse_exc_t("num-replicas is not a number");
    }

    if (dc_info->path[0] != "datacenters") {
        throw admin_parse_exc_t(data.params["datacenter"][0] + " is not a datacenter");
    }

    datacenter_id_t datacenter(dc_info->uuid);
    if (get_machine_count_in_datacenter(cluster_metadata, datacenter) < (size_t)num_replicas) {
        throw admin_cluster_exc_t("the number of replicas cannot be more than the number of machines in the datacenter");
    }

    if (ns_info->path[0] == "dummy_namespaces") {
        namespaces_semilattice_metadata_t<mock::dummy_protocol_t>::namespace_map_t::iterator i = cluster_metadata.dummy_namespaces.namespaces.find(ns_info->uuid);
        if (i == cluster_metadata.dummy_namespaces.namespaces.end()) {
            throw admin_parse_exc_t("unexpected error, namespace not found");
        } else if (i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error, namespace has been deleted");
        }
        do_admin_set_replicas_internal(i->second.get_mutable(), datacenter, num_replicas);

    } else if (ns_info->path[0] == "memcached_namespaces") {
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator i = cluster_metadata.memcached_namespaces.namespaces.find(ns_info->uuid);
        if (i == cluster_metadata.memcached_namespaces.namespaces.end()) {
            throw admin_parse_exc_t("unexpected error, namespace not found");
        } else if (i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected error, namespace has been deleted");
        }
        do_admin_set_replicas_internal(i->second.get_mutable(), datacenter, num_replicas);

    } else {
        throw admin_parse_exc_t(data.params["namespace"][0] + " is not a namespace");
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

template <class protocol_t>
void admin_cluster_link_t::do_admin_set_replicas_internal(namespace_semilattice_metadata_t<protocol_t>& ns, const datacenter_id_t& datacenter, int num_replicas) {
    if (ns.primary_datacenter.in_conflict()) {
        throw admin_cluster_exc_t("namespace primary datacenter is in conflict, run 'help resolve' for more information");
    }

    if (ns.replica_affinities.in_conflict()) {
        throw admin_cluster_exc_t("namespace replica affinities are in conflict, run 'help resolve' for more information");
    }

    if (ns.ack_expectations.in_conflict()) {
        throw admin_cluster_exc_t("namespace ack expectations are in conflict, run 'help resolve' for more information");
    }

    bool is_primary = (datacenter == ns.primary_datacenter.get_mutable());
    if (is_primary && num_replicas == 0) {
        throw admin_cluster_exc_t("the number of replicas for the primary datacenter cannot be 0");
    }

    std::map<datacenter_id_t, int>::iterator i = ns.ack_expectations.get_mutable().find(datacenter);
    if (i == ns.ack_expectations.get_mutable().end()) {
        ns.ack_expectations.get_mutable()[datacenter] = 0;
    } else if (i->second > num_replicas) {
        throw admin_cluster_exc_t("the number of replicas for this datacenter cannot be less than the number of acks, run 'help set acks' for more information");
    }

    ns.replica_affinities.get_mutable()[datacenter] = num_replicas - (is_primary ? 1 : 0);
    ns.replica_affinities.upgrade_version(change_request_id);
}

void admin_cluster_link_t::do_admin_remove(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::vector<std::string> ids = data.params["id"];
    std::string error;
    bool do_update = false;

    for (size_t i = 0; i < ids.size(); ++i) {
        try {
            metadata_info_t *obj_info = get_info_from_id(ids[i]);

            // TODO: in case of machine, check if it's up and ask for confirmation if it is

            // Remove the object from the metadata
            if (obj_info->path[0] == "machines") {
                do_admin_remove_internal(cluster_metadata.machines.machines, obj_info->uuid);
            } else if (obj_info->path[0] == "datacenters") {
                do_admin_remove_internal(cluster_metadata.datacenters.datacenters, obj_info->uuid);
            } else if (obj_info->path[0] == "dummy_namespaces") {
                do_admin_remove_internal(cluster_metadata.dummy_namespaces.namespaces, obj_info->uuid);
            } else if (obj_info->path[0] == "memcached_namespaces") {
                do_admin_remove_internal(cluster_metadata.memcached_namespaces.namespaces, obj_info->uuid);
            } else {
                throw admin_cluster_exc_t("unrecognized object type: " + obj_info->path[0]);
            }

            do_update = true;

            // Clean up any hanging references
            if (obj_info->path[0] == "machines") {
                machine_id_t machine(obj_info->uuid);
                remove_machine_pinnings(machine, cluster_metadata.memcached_namespaces.namespaces);
                remove_machine_pinnings(machine, cluster_metadata.dummy_namespaces.namespaces);
            } else if (obj_info->path[0] == "datacenters") {
                datacenter_id_t datacenter(obj_info->uuid);
                remove_datacenter_references(datacenter, cluster_metadata);
            }
        } catch (std::exception& ex) {
            error += ex.what();
            error += "\n";
        }
    }

    if (do_update) {
        fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
        if (!change_request.update(cluster_metadata)) {
            throw admin_retry_exc_t();
        }
    }

    if (!error.empty()) {
        if (ids.size() > 1) {
            throw admin_cluster_exc_t(error + "not all removes were successful");
        } else {
            throw admin_cluster_exc_t(error.substr(0, error.length() - 1));
        }
    }
}

template <class T>
void admin_cluster_link_t::do_admin_remove_internal(std::map<uuid_t, T>& obj_map, const uuid_t& key) {
    typename std::map<uuid_t, T>::iterator i = obj_map.find(key);

    if (i == obj_map.end() || i->second.is_deleted()) {
        throw admin_cluster_exc_t("object not found");
    }

    i->second.mark_deleted();
}

void admin_cluster_link_t::remove_datacenter_references(const datacenter_id_t& datacenter, cluster_semilattice_metadata_t& cluster_metadata) {
    datacenter_id_t nil_id(nil_uuid());

    // Go through machines
    for (machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (i->second.is_deleted()) {
            continue;
        }

        if (!i->second.get_mutable().datacenter.in_conflict() && i->second.get_mutable().datacenter.get_mutable() == datacenter) {
            i->second.get_mutable().datacenter.get_mutable() = nil_id;
        }
    }

    remove_datacenter_references_from_namespaces(datacenter, cluster_metadata.memcached_namespaces.namespaces);
    remove_datacenter_references_from_namespaces(datacenter, cluster_metadata.dummy_namespaces.namespaces);
}

template <class protocol_t>
void admin_cluster_link_t::remove_datacenter_references_from_namespaces(const datacenter_id_t& datacenter,
                                                                        std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t<protocol_t> > >& ns_map) {
    datacenter_id_t nil_id(nil_uuid());

    for (typename std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t<protocol_t> > >::iterator i = ns_map.begin();
         i != ns_map.end(); ++i) {
        if (i->second.is_deleted()) {
            continue;
        }

        namespace_semilattice_metadata_t<protocol_t>& ns = i->second.get_mutable();

        if (!ns.primary_datacenter.in_conflict() && ns.primary_datacenter.get_mutable() == datacenter) {
            ns.primary_datacenter.get_mutable() = nil_id;
            ns.primary_datacenter.upgrade_version(change_request_id);
        }

        if (!ns.replica_affinities.in_conflict()) {
            ns.replica_affinities.get_mutable().erase(datacenter);
            ns.replica_affinities.upgrade_version(change_request_id);
        }

        if (!ns.ack_expectations.in_conflict()) {
            ns.ack_expectations.get_mutable().erase(datacenter);
            ns.ack_expectations.upgrade_version(change_request_id);
        }
    }
}

template <class protocol_t>
void admin_cluster_link_t::list_single_namespace(const namespace_id_t& ns_id,
                                                 namespace_semilattice_metadata_t<protocol_t>& ns,
                                                 cluster_semilattice_metadata_t& cluster_metadata,
                                                 const std::string& protocol) {
    if (ns.name.in_conflict() || ns.name.get_mutable().empty()) {
        printf("namespace %s\n", uuid_to_str(ns_id).c_str());
    } else {
        printf("namespace '%s' %s\n", ns.name.get_mutable().c_str(), uuid_to_str(ns_id).c_str());
    }

    // Print primary datacenter
    if (!ns.primary_datacenter.in_conflict()) {
        datacenters_semilattice_metadata_t::datacenter_map_t::iterator dc = cluster_metadata.datacenters.datacenters.find(ns.primary_datacenter.get());
        if (dc == cluster_metadata.datacenters.datacenters.end() ||
            dc->second.is_deleted() ||
            dc->second.get_mutable().name.in_conflict() ||
            dc->second.get_mutable().name.get().empty()) {
            printf("primary datacenter %s\n", uuid_to_str(ns.primary_datacenter.get()).c_str());
        } else {
            printf("primary datacenter '%s' %s\n", dc->second.get_mutable().name.get().c_str(), uuid_to_str(ns.primary_datacenter.get()).c_str());
        }
    } else {
        printf("primary datacenter <conflict>\n");
    }

    // Print port
    if (ns.port.in_conflict()) {
        printf("running %s protocol on port <conflict>\n", protocol.c_str());
    } else {
        printf("running %s protocol on port %i\n", protocol.c_str(), ns.port.get());
    }
    printf("\n");

    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;

    // Print configured affinities and ack expectations
    delta.push_back("uuid");
    delta.push_back("name");
    delta.push_back("replicas");
    delta.push_back("acks");
    table.push_back(delta);

    for (datacenters_semilattice_metadata_t::datacenter_map_t::iterator i = cluster_metadata.datacenters.datacenters.begin();
         i != cluster_metadata.datacenters.datacenters.end(); ++i) {
        if (!i->second.is_deleted()) {
            bool affinity = false;
            delta.clear();
            delta.push_back(uuid_to_str(i->first));
            delta.push_back(i->second.get_mutable().name.in_conflict() ? "<conflict>" : i->second.get_mutable().name.get());

            if (!ns.primary_datacenter.in_conflict() && ns.primary_datacenter.get() == i->first) {
                if (ns.replica_affinities.get_mutable().count(i->first) == 1) {
                    delta.push_back(strprintf("%i", ns.replica_affinities.get_mutable()[i->first] + 1));
                } else {
                    delta.push_back("1");
                }

                affinity = true;
            } else if (ns.replica_affinities.get_mutable().count(i->first) == 1) {
                delta.push_back(strprintf("%i", ns.replica_affinities.get_mutable()[i->first]));
                affinity = true;
            } else {
                delta.push_back("0");
            }

            if (ns.ack_expectations.get_mutable().count(i->first) == 1) {
                delta.push_back(strprintf("%i", ns.ack_expectations.get_mutable()[i->first]));
                affinity = true;
            } else {
                delta.push_back("0");
            }

            if (affinity) {
                table.push_back(delta);
            }
        }
    }

    printf("affinity with %ld datacenter%s\n", table.size() - 1, table.size() == 2 ? "" : "s");
    if (table.size() > 1) {
        admin_print_table(table);
    }
    printf("\n");

    if (ns.shards.in_conflict()) {
        printf("shards in conflict\n");
    } else if (ns.blueprint.in_conflict()) {
        printf("cluster blueprint in conflict\n");
    } else {
        // Print shard hosting
        table.clear();
        delta.clear();
        delta.push_back("shard");
        delta.push_back("machine uuid");
        delta.push_back("name");
        delta.push_back("primary");
        table.push_back(delta);

        add_single_namespace_replicas(ns.shards.get_mutable(),
                                      ns.blueprint.get_mutable(),
                                      cluster_metadata.machines.machines,
                                      table);

        printf("%ld replica%s for %ld shard%s\n",
               table.size() - 1, table.size() == 2 ? "" : "s",
               ns.shards.get_mutable().size(), ns.shards.get_mutable().size() == 1 ? "" : "s");
        if (table.size() > 1) {
            admin_print_table(table);
        }
    }
}

template <class protocol_t>
void admin_cluster_link_t::add_single_namespace_replicas(std::set<typename protocol_t::region_t>& shards,
                                                         persistable_blueprint_t<protocol_t>& blueprint,
                                                         machines_semilattice_metadata_t::machine_map_t& machine_map,
                                                         std::vector<std::vector<std::string> >& table) {
    std::vector<std::string> delta;

    for (typename std::set<typename protocol_t::region_t>::iterator s = shards.begin(); s != shards.end(); ++s) {
        std::string shard_str(admin_value_to_string(*s));

        // First add the primary host
        for (typename persistable_blueprint_t<protocol_t>::role_map_t::iterator i = blueprint.machines_roles.begin();
             i != blueprint.machines_roles.end(); ++i) {
            typename persistable_blueprint_t<protocol_t>::region_to_role_map_t::iterator j = i->second.find(*s);
            if (j != i->second.end() && j->second == blueprint_details::role_primary) {
                delta.clear();
                delta.push_back(shard_str);
                delta.push_back(uuid_to_str(i->first));

                machines_semilattice_metadata_t::machine_map_t::iterator m = machine_map.find(i->first);
                if (m == machine_map.end() || m->second.is_deleted()) {
                    // This shouldn't really happen, but oh well
                    delta.push_back(std::string());
                } else if (m->second.get_mutable().name.in_conflict()) {
                    delta.push_back("<conflict>");
                } else {
                    delta.push_back(m->second.get_mutable().name.get());
                }

                delta.push_back("yes");
                table.push_back(delta);
            }
        }

        // Then add all the secondaries
        for (typename persistable_blueprint_t<protocol_t>::role_map_t::iterator i = blueprint.machines_roles.begin();
             i != blueprint.machines_roles.end(); ++i) {
            typename persistable_blueprint_t<protocol_t>::region_to_role_map_t::iterator j = i->second.find(*s);
            if (j != i->second.end() && j->second == blueprint_details::role_secondary) {
                delta.clear();
                delta.push_back(shard_str);
                delta.push_back(uuid_to_str(i->first));

                machines_semilattice_metadata_t::machine_map_t::iterator m = machine_map.find(i->first);
                if (m == machine_map.end() || m->second.is_deleted()) {
                    // This shouldn't really happen, but oh well
                    delta.push_back(std::string());
                } else if (m->second.get_mutable().name.in_conflict()) {
                    delta.push_back("<conflict>");
                } else {
                    delta.push_back(m->second.get_mutable().name.get());
                }

                delta.push_back("no");
                table.push_back(delta);
            }
        }
    }
}

void admin_cluster_link_t::list_single_datacenter(const datacenter_id_t& dc_id,
                                                  datacenter_semilattice_metadata_t& dc,
                                                  cluster_semilattice_metadata_t& cluster_metadata) {
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> delta;
    if (dc.name.in_conflict() || dc.name.get_mutable().empty()) {
        printf("datacenter %s\n", uuid_to_str(dc_id).c_str());
    } else {
        printf("datacenter '%s' %s\n", dc.name.get_mutable().c_str(), uuid_to_str(dc_id).c_str());
    }
    printf("\n");

    // Get a list of machines in the datacenter
    delta.push_back("uuid");
    delta.push_back("name");
    table.push_back(delta);

    for (machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted() &&
            !i->second.get_mutable().datacenter.in_conflict() &&
            i->second.get_mutable().datacenter.get() == dc_id) {
            delta.clear();
            delta.push_back(uuid_to_str(i->first));
            delta.push_back(i->second.get_mutable().name.in_conflict() ? "<conflict>" : i->second.get_mutable().name.get());
            table.push_back(delta);
        }
    }

    printf("%ld machine%s\n", table.size() - 1, table.size() == 2 ? "" : "s");
    if (table.size() > 1) {
        admin_print_table(table);
    }
    printf("\n");

    // Get a list of namespaces hosted by the datacenter
    table.clear();
    delta.clear();
    delta.push_back("uuid");
    delta.push_back("name");
    delta.push_back("protocol");
    delta.push_back("primary");
    delta.push_back("replicas");
    table.push_back(delta);

    add_single_datacenter_affinities(dc_id, cluster_metadata.dummy_namespaces.namespaces, table, "dummy");
    add_single_datacenter_affinities(dc_id, cluster_metadata.memcached_namespaces.namespaces, table, "memcached");

    printf("%ld namespace%s\n", table.size() - 1, table.size() == 2 ? "" : "s");
    if (table.size() > 1) {
        admin_print_table(table);
    }
}

template <class map_type>
void admin_cluster_link_t::add_single_datacenter_affinities(const datacenter_id_t& dc_id, map_type& ns_map, std::vector<std::vector<std::string> >& table, const std::string& protocol) {
    std::vector<std::string> delta;

    for (typename map_type::iterator i = ns_map.begin(); i != ns_map.end(); ++i) {
        if (!i->second.is_deleted()) {
            typename map_type::mapped_type::value_t& ns = i->second.get_mutable();
            size_t replicas(0);

            delta.clear();
            delta.push_back(uuid_to_str(i->first));
            delta.push_back(ns.name.in_conflict() ? "<conflict>" : ns.name.get());
            delta.push_back(protocol);

            // TODO: this will only list the replicas required by the user, not the actual replicas (in case of impossible requirements)
            if (!ns.primary_datacenter.in_conflict() &&
                ns.primary_datacenter.get() == dc_id) {
                delta.push_back("yes");
                ++replicas;
            } else {
                delta.push_back("no");
            }

            if (!ns.replica_affinities.in_conflict() &&
                ns.replica_affinities.get_mutable().count(dc_id) == 1) {
                replicas += ns.replica_affinities.get_mutable()[dc_id];
            }

            delta.push_back(strprintf("%ld", replicas));

            if (replicas > 0) {
                table.push_back(delta);
            }
        }
    }
}

void admin_cluster_link_t::list_single_machine(const machine_id_t& machine_id,
                                               machine_semilattice_metadata_t& machine,
                                               cluster_semilattice_metadata_t& cluster_metadata) {
    if (machine.name.in_conflict() || machine.name.get_mutable().empty()) {
        printf("machine %s\n", uuid_to_str(machine_id).c_str());
    } else {
        printf("machine '%s' %s\n", machine.name.get_mutable().c_str(), uuid_to_str(machine_id).c_str());
    }

    // Print datacenter
    if (!machine.datacenter.in_conflict()) {
        datacenters_semilattice_metadata_t::datacenter_map_t::iterator dc = cluster_metadata.datacenters.datacenters.find(machine.datacenter.get());
        if (dc == cluster_metadata.datacenters.datacenters.end() ||
            dc->second.is_deleted() ||
            dc->second.get_mutable().name.in_conflict() ||
            dc->second.get_mutable().name.get().empty()) {
            printf("in datacenter %s\n", uuid_to_str(machine.datacenter.get()).c_str());
        } else {
            printf("in datacenter '%s' %s\n", dc->second.get_mutable().name.get().c_str(), uuid_to_str(machine.datacenter.get()).c_str());
        }
    } else {
        printf("in datacenter <conflict>\n");
    }
    printf("\n");

    // Print hosted replicas
    std::vector<std::vector<std::string> > table;
    std::vector<std::string> header;

    header.push_back("namespace");
    header.push_back("name");
    header.push_back("shard");
    header.push_back("primary");
    table.push_back(header);

    size_t namespace_count = 0;
    namespace_count += add_single_machine_replicas(machine_id, cluster_metadata.dummy_namespaces.namespaces, table);
    namespace_count += add_single_machine_replicas(machine_id, cluster_metadata.memcached_namespaces.namespaces, table);

    printf("hosting %ld replica%s from %ld namespace%s\n", table.size() - 1, table.size() == 2 ? "" : "s", namespace_count, namespace_count == 1 ? "" : "s");
    if (table.size() > 1) {
        admin_print_table(table);
    }
}

template <class map_type>
size_t admin_cluster_link_t::add_single_machine_replicas(const machine_id_t& machine_id,
                                                         map_type& ns_map,
                                                         std::vector<std::vector<std::string> >& table) {
    size_t matches(0);

    for (typename map_type::iterator i = ns_map.begin(); i != ns_map.end(); ++i) {
        // TODO: if there is a blueprint conflict, the values won't be accurate
        if (!i->second.is_deleted() && !i->second.get_mutable().blueprint.in_conflict()) {
            typename map_type::mapped_type::value_t& ns = i->second.get_mutable();
            std::string uuid(uuid_to_str(i->first));
            std::string name(ns.name.in_conflict() ? "<conflict>" : ns.name.get_mutable());
            matches += add_single_machine_blueprint(machine_id, ns.blueprint.get_mutable(), table, uuid, name);
        }
    }

    return matches;
}

template <class protocol_t>
bool admin_cluster_link_t::add_single_machine_blueprint(const machine_id_t& machine_id,
                                                        persistable_blueprint_t<protocol_t>& blueprint,
                                                        std::vector<std::vector<std::string> >& table,
                                                        const std::string& ns_uuid,
                                                        const std::string& ns_name) {
    std::vector<std::string> delta;
    bool match(false);

    typename persistable_blueprint_t<protocol_t>::role_map_t::iterator machine_entry = blueprint.machines_roles.find(machine_id);
    if (machine_entry == blueprint.machines_roles.end()) {
        return false;
    }

    for (typename persistable_blueprint_t<protocol_t>::region_to_role_map_t::iterator i = machine_entry->second.begin();
         i != machine_entry->second.end(); ++i) {
        if (i->second == blueprint_details::role_primary || i->second == blueprint_details::role_secondary) {
            delta.clear();
            delta.push_back(ns_uuid);
            delta.push_back(ns_name);

            // Build a string for the shard
            delta.push_back(admin_value_to_string(i->first));

            if (i->second == blueprint_details::role_primary) {
                delta.push_back("yes");
            } else {
                delta.push_back("no");
            }

            table.push_back(delta);
            match = true;
        } else {
            continue;
        }
    }


    return match;
}

void admin_cluster_link_t::do_admin_resolve(admin_command_parser_t::command_data& data) {
    metadata_change_handler_t<cluster_semilattice_metadata_t>::metadata_change_request_t
        change_request(&mailbox_manager, choose_sync_peer());
    cluster_semilattice_metadata_t cluster_metadata = change_request.get();
    std::string obj_id = data.params["id"][0];
    std::string field = data.params["field"][0];
    metadata_info_t *obj_info = get_info_from_id(obj_id);

    if (obj_info->path[0] == "machines") {
        machines_semilattice_metadata_t::machine_map_t::iterator i = cluster_metadata.machines.machines.find(obj_info->uuid);
        if (i == cluster_metadata.machines.machines.end() || i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected exception when looking up object: " + obj_id);
        }
        resolve_machine_value(i->second.get_mutable(), field);
    } else if (obj_info->path[0] == "datacenters") {
        datacenters_semilattice_metadata_t::datacenter_map_t::iterator i = cluster_metadata.datacenters.datacenters.find(obj_info->uuid);
        if (i == cluster_metadata.datacenters.datacenters.end() || i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected exception when looking up object: " + obj_id);
        }
        resolve_datacenter_value(i->second.get_mutable(), field);
    } else if (obj_info->path[0] == "dummy_namespaces") {
        namespaces_semilattice_metadata_t<mock::dummy_protocol_t>::namespace_map_t::iterator i = cluster_metadata.dummy_namespaces.namespaces.find(obj_info->uuid);
        if (i == cluster_metadata.dummy_namespaces.namespaces.end() || i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected exception when looking up object: " + obj_id);
        }
        resolve_namespace_value(i->second.get_mutable(), field);
    } else if (obj_info->path[0] == "memcached_namespaces") {
        namespaces_semilattice_metadata_t<memcached_protocol_t>::namespace_map_t::iterator i = cluster_metadata.memcached_namespaces.namespaces.find(obj_info->uuid);
        if (i == cluster_metadata.memcached_namespaces.namespaces.end() || i->second.is_deleted()) {
            throw admin_cluster_exc_t("unexpected exception when looking up object: " + obj_id);
        }
        resolve_namespace_value(i->second.get_mutable(), field);
    } else {
        throw admin_cluster_exc_t("unexpected object type encountered: " + obj_info->path[0]);
    }

    fill_in_blueprints(&cluster_metadata, directory_read_manager.get_root_view()->get(), change_request_id);
    if (!change_request.update(cluster_metadata)) {
        throw admin_retry_exc_t();
    }
}

// Reads from stream until a newline is occurred.  Reads the newline
// but does not store it in out.  Returns true upon success.
bool getline(FILE *stream, std::string *out) {
    out->clear();

    const int size = 1024;
    char buf[size];

    for (;;) {
        char *res = fgets(buf, size, stream);

        if (!res) {
            return false;
        }

        if (res) {
            int len = strlen(buf);
            guarantee(len < size);
            guarantee(len > 0);

            if (buf[len - 1] == '\n') {
                buf[len - 1] = '\0';
                out->append(buf);
                return true;
            } else {
                out->append(buf);
            }
        }
    }
}

template <class T>
void admin_cluster_link_t::resolve_value(vclock_t<T>& field) {
    if (!field.in_conflict()) {
        throw admin_cluster_exc_t("value is not in conflict");
    }

    std::vector<T> values = field.get_all_values();

    printf("%ld values\n", values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        printf(" %ld: %s\n", i + 1, admin_value_to_string(values[i]).c_str());
    }
    printf(" 0: cancel\n");
    printf("select: ");

    std::string selection;
    bool getline_res = getline(stdin, &selection);
    if (!getline_res) {
        throw admin_cluster_exc_t("could not read from stdin");
    }

    uint64_t index;
    if (!strtou64_strict(selection, 10, &index) || index > values.size()) {
        throw admin_cluster_exc_t("invalid selection");
    } else if (index == 0) {
        throw admin_cluster_exc_t("cancelled");
    } else if (index != 0) {
        field = field.make_resolving_version(values[index - 1], change_request_id);
    }
}

void admin_cluster_link_t::resolve_machine_value(machine_semilattice_metadata_t& machine,
                                                 const std::string& field) {
    if (field == "name") {
        resolve_value(machine.name);
    } else if (field == "datacenter") {
        resolve_value(machine.datacenter);
    } else {
        throw admin_cluster_exc_t("unknown machine field: " + field);
    }
}

void admin_cluster_link_t::resolve_datacenter_value(datacenter_semilattice_metadata_t& dc,
                                                    const std::string& field) {
    if (field == "name") {
        resolve_value(dc.name);
    } else {
        throw admin_cluster_exc_t("unknown datacenter field: " + field);
    }
}

template <class protocol_t>
void admin_cluster_link_t::resolve_namespace_value(namespace_semilattice_metadata_t<protocol_t>& ns,
                                                   const std::string& field) {
    if (field == "name") {
        resolve_value(ns.name);
    } else if (field == "datacenter") {
        resolve_value(ns.primary_datacenter);
    } else if (field == "replicas") {
        resolve_value(ns.replica_affinities);
    } else if (field == "acks") {
        resolve_value(ns.ack_expectations);
    } else if (field == "shards") {
        resolve_value(ns.shards);
    } else if (field == "port") {
        resolve_value(ns.port);
    } else if (field == "master_pinnings") {
        resolve_value(ns.primary_pinnings);
    } else if (field == "replica_pinnings") {
        resolve_value(ns.secondary_pinnings);
    } else {
        throw admin_cluster_exc_t("unknown namespace field: " + field);
    }
}

size_t admin_cluster_link_t::machine_count() const {
    size_t count = 0;
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();

    for (machines_semilattice_metadata_t::machine_map_t::const_iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted()) {
            ++count;
        }
    }

    return count;
}

size_t admin_cluster_link_t::get_machine_count_in_datacenter(const cluster_semilattice_metadata_t& cluster_metadata, const datacenter_id_t& datacenter) {
    size_t count = 0;

    for (machines_semilattice_metadata_t::machine_map_t::const_iterator i = cluster_metadata.machines.machines.begin();
         i != cluster_metadata.machines.machines.end(); ++i) {
        if (!i->second.is_deleted() &&
            !i->second.get().datacenter.in_conflict() &&
            i->second.get().datacenter.get() == datacenter) {
            ++count;
        }
    }

    return count;
}

size_t admin_cluster_link_t::available_machine_count() {
    std::map<peer_id_t, cluster_directory_metadata_t> directory = directory_read_manager.get_root_view()->get();
    cluster_semilattice_metadata_t cluster_metadata = semilattice_metadata->get();
    size_t count = 0;

    for (std::map<peer_id_t, cluster_directory_metadata_t>::iterator i = directory.begin(); i != directory.end(); i++) {
        // Check uuids vs machines in cluster
        machines_semilattice_metadata_t::machine_map_t::const_iterator machine = cluster_metadata.machines.machines.find(i->second.machine_id);
        if (machine != cluster_metadata.machines.machines.end() && !machine->second.is_deleted()) {
            ++count;
        }
    }

    return count;
}

size_t admin_cluster_link_t::issue_count() {
    return admin_tracker.issue_aggregator.get_issues().size();
}

std::string admin_cluster_link_t::path_to_str(const std::vector<std::string>& path) {
    if (path.size() == 0) {
        return std::string();
    }

    std::string result(path[0]);
    for (size_t i = 1; i < path.size(); ++i) {
        result += "/" + path[i];
    }

    return result;
}
