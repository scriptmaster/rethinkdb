// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/cluster_config.hpp"

#include "clustering/administration/datum_adapter.hpp"

cluster_config_artificial_table_backend_t::cluster_config_artificial_table_backend_t(
        boost::shared_ptr< semilattice_readwrite_view_t<
            auth_semilattice_metadata_t> > _sl_view) :
    auth_doc(_sl_view) {
    docs["auth"] = &auth_doc;
}

std::string cluster_config_artificial_table_backend_t::get_primary_key_name() {
    return "id";
}

bool cluster_config_artificial_table_backend_t::read_all_primary_keys(
        UNUSED signal_t *interruptor,
        std::vector<ql::datum_t> *keys_out,
        UNUSED std::string *error_out) {
    keys_out->clear();
    for (auto it = docs.begin(); it != docs.end(); ++it) {
        keys_out->push_back(ql::datum_t(datum_string_t(it->first)));
    }
    return true;
}

bool cluster_config_artificial_table_backend_t::read_row(
        ql::datum_t primary_key,
        signal_t *interruptor,
        ql::datum_t *row_out,
        std::string *error_out) {
    if (primary_key.get_type() != ql::datum_t::R_STR) {
        *row_out = ql::datum_t();
        return true;
    }
    auto it = docs.find(primary_key.as_str().to_std());
    if (it == docs.end()) {
        *row_out = ql::datum_t();
        return true;
    }
    return it->second->read(interruptor, row_out, error_out);
}

bool cluster_config_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        ql::datum_t new_value,
        signal_t *interruptor,
        std::string *error_out) {
    if (!new_value.has()) {
        *error_out = "It's illegal to delete rows from the `rethinkdb.cluster_config` "
            "table.";
        return false;
    }
    const char *missing_message = "It's illegal to insert new rows into the "
        "`rethinkdb.cluster_config` table.";
    if (primary_key.get_type() != ql::datum_t::R_STR) {
        *error_out = missing_message;
        return false;
    }
    auto it = docs.find(primary_key.as_str().to_std());
    if (it == docs.end()) {
        *error_out = missing_message;
        return false;
    }
    return it->second->write(interruptor, new_value, error_out);
}

ql::datum_t make_hidden_auth_key_datum() {
    ql::datum_object_builder_t builder;
    builder.overwrite("hidden", ql::datum_t::boolean(true));
    return std::move(builder).to_datum();
}

ql::datum_t convert_auth_key_to_datum(
        const auth_key_t &value) {
    if (value.str().empty()) {
        return ql::datum_t::null();
    } else {
        return make_hidden_auth_key_datum();
    }
}

bool convert_auth_key_from_datum(
        ql::datum_t datum,
        auth_key_t *value_out,
        std::string *error_out) {
    if (datum->get_type() == ql::datum_t::R_NULL) {
        *value_out = auth_key_t();
        return true;
    } else if (datum->get_type() == ql::datum_t::R_STR) {
        if (!value_out->assign_value(datum->as_str().to_std())) {
            if (datum->as_str().size() > static_cast<size_t>(auth_key_t::max_length)) {
                *error_out = strprintf("The auth key should be at most %zu bytes long, "
                    "but your given key is %zu bytes.",
                    static_cast<size_t>(auth_key_t::max_length), datum->as_str().size());
            } else {
                /* Currently this can't happen, because length is the only reason to
                invalidate an auth key. This is here for future-proofing. */
                *error_out = "The given auth key is invalid.";
            }
            return false;
        }
        return true;
    } else if (datum == make_hidden_auth_key_datum()) {
        *error_out = "You're trying to set the `auth_key` field in the `auth` document "
            "of `rethinkdb.cluster_config` to {hidden: true}. The `auth_key` field "
            "can be set to a string, or `null` for no auth key. {hidden: true} is a "
            "special place-holder value that RethinkDB returns if you try to read the "
            "auth key; RethinkDB won't show you the real auth key for security reasons. "
            "Setting the auth key to {hidden: true} is not allowed.";
        return false;
    } else {
        *error_out = "Expected a string or null; got " + datum->print();
        return false;
    }
}

bool cluster_config_artificial_table_backend_t::auth_doc_t::read(
        UNUSED signal_t *interruptor,
        ql::datum_t *row_out,
        UNUSED std::string *error_out) {
    on_thread_t thread_switcher(sl_view->home_thread());
    ql::datum_object_builder_t obj_builder;
    obj_builder.overwrite("id", ql::datum_t("auth"));
    obj_builder.overwrite("auth_key", convert_auth_key_to_datum(
        sl_view->get().auth_key.get_ref()));
    *row_out = std::move(obj_builder).to_datum();
    return true;
}

bool cluster_config_artificial_table_backend_t::auth_doc_t::write(
        UNUSED signal_t *interruptor,
        const ql::datum_t &row,
        std::string *error_out) {
    converter_from_datum_object_t converter;
    std::string dummy_error;
    if (!converter.init(row, &dummy_error)) {
        crash("artificial_table_t should guarantee input is an object");
    }
    ql::datum_t dummy_pkey;
    if (!converter.get("id", &dummy_pkey, &dummy_error)) {
        crash("artificial_table_t should guarantee primary key is present and correct");
    }

    ql::datum_t auth_key_datum;
    if (!converter.get("auth_key", &auth_key_datum, error_out)) {
        return false;
    }
    auth_key_t auth_key;
    if (!convert_auth_key_from_datum(auth_key_datum, &auth_key, error_out)) {
        return false;
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    {
        on_thread_t thread_switcher(sl_view->home_thread());
        auth_semilattice_metadata_t md = sl_view->get();
        md.auth_key.set(auth_key);
        sl_view->join(md);
    }

    return true;
}

