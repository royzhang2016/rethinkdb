// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/servers/config_client.hpp"

server_config_client_t::server_config_client_t(
        mailbox_manager_t *_mailbox_manager,
        watchable_map_t<peer_id_t, cluster_directory_metadata_t> *_directory_view,
        watchable_map_t<std::pair<peer_id_t, server_id_t>, empty_value_t>
            *_peer_connections_map) :
    mailbox_manager(_mailbox_manager),
    directory_view(_directory_view),
    peer_connections_map(_peer_connections_map),
    directory_subs(
        directory_view,
        std::bind(&server_config_client_t::on_directory_change, this, ph::_1, ph::_2),
        true),
    peer_connections_map_subs(
        peer_connections_map,
        std::bind(&server_config_client_t::on_peer_connections_map_change,
            this, ph::_1, ph::_2),
        true)
    { }

bool server_config_client_t::set_config(const server_id_t &server_id,
        const server_config_t &new_config, signal_t *interruptor) {
    bool name_collision = false;
    server_config_map.read_all(
    [&](const server_id_t &sid, const server_config_versioned_t *conf) {
        if (sid != server_id && conf->config.name == new_config.name) {
            name_collision = true;
        }
    });
    if (name_collision) {
        return false;
    }

    boost::optional<peer_id_t> peer = server_to_peer_map.get_key(server_id);
    if (!static_cast<bool>(peer)) {
        return false;
    }
    server_config_business_card_t bcard;
    directory_view->read_key(*peer, [&](const cluster_directory_metadata_t *md) {
        guarantee(md != nullptr);
        bcard = md->server_config_business_card.get();
    });

    uint64_t version;
    {
        promise_t<std::pair<uint64_t, std::string> > reply;
        mailbox_t<void(uint64_t, std::string)> ack_mailbox(
            mailbox_manager,
            [&](signal_t *, uint64_t v, const std::string &m) {
                reply.pulse(std::make_pair(v, m));
            });
        disconnect_watcher_t disconnect_watcher(mailbox_manager, *peer);
        send(mailbox_manager, bcard.set_config_addr,
            new_config, ack_mailbox.get_address());
        wait_any_t waiter(reply.get_ready_signal(), &disconnect_watcher);
        wait_interruptible(&waiter, interruptor);
        if (!reply.is_pulsed()) {
            return false;
        }
        if (!reply.assert_get_value().second.empty()) {
            guarantee(reply.assert_get_value().first == 0);
            return false;
        }
        version = reply.assert_get_value().first;
    }

    /* Wait up to 10 seconds for the change to appear in the directory. */
    try {
        signal_timer_t timeout;
        timeout.start(10000);
        wait_any_t waiter(interruptor, &timeout);
        server_config_map.run_key_until_satisfied(
            server_id,
            [&](const server_config_versioned_t *conf) {
                return conf == nullptr || conf->version >= version;
            },
            &waiter);
    } catch (const interrupted_exc_t &) {
        if (interruptor->is_pulsed()) {
            throw;
        }
    }

    return true;
}

void server_config_client_t::install_server_metadata(
        const peer_id_t &peer_id,
        const cluster_directory_metadata_t &metadata) {
    const server_id_t &server_id = metadata.server_id;
    server_to_peer_map.set_key(server_id, peer_id);
    peer_connections_map->read_all(
        [&](const std::pair<peer_id_t, server_id_t> &pair, const empty_value_t *) {
            if (pair.first == peer_id) {
                connections_map.set_key(
                    std::make_pair(server_id, pair.second), empty_value_t());
            }
        });
    server_config_map.set_key(server_id, metadata.server_config);
}

void server_config_client_t::on_directory_change(
        const peer_id_t &peer_id,
        const cluster_directory_metadata_t *metadata) {
    if (metadata != nullptr) {
        if (metadata->peer_type != SERVER_PEER) {
            return;
        }
        const server_id_t &server_id = metadata->server_id;
        if (!static_cast<bool>(peer_to_server_map.get_key(peer_id))) {
            all_server_to_peer_map.insert(std::make_pair(server_id, peer_id));
            peer_to_server_map.set_key(peer_id, server_id);
            install_server_metadata(peer_id, *metadata);
        } else {
            server_config_map.set_key(server_id, metadata->server_config);
        }

    } else {
        boost::optional<server_id_t> server_id = peer_to_server_map.get_key(peer_id);
        if (!static_cast<bool>(server_id)) {
            return;
        }
        for (auto it = all_server_to_peer_map.lower_bound(*server_id); ; ++it) {
            guarantee(it != all_server_to_peer_map.end());
            if (it->second == peer_id) {
                all_server_to_peer_map.erase(it);
                break;
            }
        }
        peer_to_server_map.delete_key(peer_id);
        peer_connections_map->read_all(
            [&](const std::pair<peer_id_t, server_id_t> &pair, const empty_value_t *) {
                if (pair.first == peer_id) {
                    connections_map.delete_key(std::make_pair(*server_id, pair.second));
                }
            });
        server_config_map.delete_key(*server_id);

        /* If there is another connected peer with the same server ID, reinstall its
        values. */
        auto jt = all_server_to_peer_map.find(*server_id);
        if (jt != all_server_to_peer_map.end()) {
            directory_view->read_key(jt->second,
                [&](const cluster_directory_metadata_t *other_metadata) {
                    guarantee(other_metadata != nullptr);
                    guarantee(other_metadata->server_id == *server_id);
                    install_server_metadata(jt->second, *other_metadata);
                });
        }
    }
}

void server_config_client_t::on_peer_connections_map_change(
        const std::pair<peer_id_t, server_id_t> &key,
        const empty_value_t *value) {
    directory_view->read_key(key.first,
    [&](const cluster_directory_metadata_t *metadata) {
        if (metadata != nullptr) {
            if (value != nullptr) {
                connections_map.set_key(
                    std::make_pair(metadata->server_id, key.second), empty_value_t());
            } else {
                connections_map.delete_key(
                    std::make_pair(metadata->server_id, key.second));
            }
        }
    });
}


