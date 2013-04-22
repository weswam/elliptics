#include "session_indexes.hpp"
#include "callback_p.h"
#include "functional_p.h"

namespace ioremap { namespace elliptics {

static dnet_id indexes_generate_id(session &sess, const dnet_id &data_id)
{
	// TODO: Better id for storing the tree?
	std::string key;
	key.reserve(sizeof(data_id.id) + 5);
	key.resize(sizeof(data_id.id));
	memcpy(&key[0], data_id.id, sizeof(data_id.id));
	key += "index";

	dnet_id id;
	sess.transform(key, id);
	id.group_id = 0;
	id.type = 0;

	return id;
}

struct update_indexes_functor : public std::enable_shared_from_this<update_indexes_functor>
{
	ELLIPTICS_DISABLE_COPY(update_indexes_functor)

	typedef std::function<void (const std::exception_ptr &)> handler_func;

	enum update_index_action {
		insert_data,
		remove_data
	};

	update_indexes_functor(session &sess, const handler_func &handler, const key &request_id,
		const std::vector<index_entry> &input_indexes, const dnet_id &id)
		: sess(sess), handler(handler), request_id(request_id), id(id), finished(0)
	{
		indexes.indexes = input_indexes;
		std::sort(indexes.indexes.begin(), indexes.indexes.end(), dnet_raw_id_less_than<>());
		msgpack::pack(buffer, indexes);
	}

	session sess;
	std::function<void (const std::exception_ptr &)> handler;
	key request_id;
	data_pointer request_data;
	// indexes to set
	dnet_indexes indexes;
	dnet_id id;

	msgpack::sbuffer buffer;
	// currently set indexes
	dnet_indexes remote_indexes;
	std::mutex previous_data_mutex;
	std::map<dnet_raw_id, data_pointer, dnet_raw_id_less_than<>> previous_data;
	std::vector<index_entry> inserted_ids;
	std::vector<index_entry> removed_ids;
	std::vector<dnet_raw_id> success_inserted_ids;
	std::vector<dnet_raw_id> success_removed_ids;
	std::mutex mutex;
	size_t finished;
	std::exception_ptr exception;

	/*!
	 * Update data-object table for secondary certain index.
	 */
	template <update_index_action action>
	data_pointer convert_index_table(const data_pointer &index_data, const data_pointer &data)
	{
		dnet_indexes indexes;
		if (!data.empty())
			indexes_unpack(data, &indexes, "update_functor");

		// Construct index entry
		index_entry request_index;
		request_index.index = request_id.raw_id();
		request_index.data = index_data;

		auto it = std::lower_bound(indexes.indexes.begin(), indexes.indexes.end(),
			request_index, dnet_raw_id_less_than<skip_data>());
		if (it != indexes.indexes.end() && it->index == request_index.index) {
			// It's already there
			if (action == insert_data) {
				if (it->data == request_index.data) {
					// All's ok, keep it untouched
					return data;
				} else {
					// Data is not correct, remember current value due to possible rollback
					std::lock_guard<std::mutex> lock(previous_data_mutex);
					previous_data[it->index] = it->data;
					it->data = request_index.data;
				}
			} else {
				// Anyway, destroy it
				indexes.indexes.erase(it);
			}
		} else {
			// Index is not created yet
			if (action == insert_data) {
				// Just insert it
				indexes.indexes.insert(it, 1, request_index);
			} else {
				// All's ok, keep it untouched
				return data;
			}
		}

		msgpack::sbuffer buffer;
		msgpack::pack(&buffer, indexes);
		return data_pointer::copy(buffer.data(), buffer.size());
	}

	/*!
	 * All changes were reverted - succesfully or not.
	 * Anyway, notify the user.
	 */
	void on_index_table_revert_finished()
	{
		if (finished != success_inserted_ids.size() + success_removed_ids.size())
			return;

		handler(exception);
	}

	/*!
	 * Reverting of certain index was finished with error \a err.
	 */
	void on_index_table_reverted(const error_info &err)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++finished;

		if (err) {
			try {
				err.throw_error();
			} catch (...) {
				exception = std::current_exception();
			}
		}

		on_index_table_revert_finished();
	}

	/*!
	 * All indexes are updated, if one of the update is failed,
	 * all successfull changes must be reverted.
	 */
	void on_index_table_update_finished()
	{
		if (finished != inserted_ids.size() + removed_ids.size())
			return;

		finished = 0;

		dnet_id tmp_id;
		memset(&tmp_id, 0, sizeof(tmp_id));

		if (success_inserted_ids.size() != inserted_ids.size()
			|| success_removed_ids.size() != removed_ids.size()) {

			if (success_inserted_ids.empty() && success_removed_ids.empty()) {
				handler(exception);
				return;
			}

			for (size_t i = 0; i < success_inserted_ids.size(); ++i) {
				const auto &remote_id = success_inserted_ids[i];
				memcpy(tmp_id.id, remote_id.id, sizeof(tmp_id.id));
				sess.write_cas(tmp_id,
					std::bind(&update_indexes_functor::convert_index_table<remove_data>,
						shared_from_this(),
						data_pointer(),
						std::placeholders::_1),
				0).connect(std::bind(&update_indexes_functor::on_index_table_reverted,
					shared_from_this(),
					std::placeholders::_2));
			}

			for (size_t i = 0; i < success_removed_ids.size(); ++i) {
				const auto &remote_id = success_removed_ids[i];
				memcpy(tmp_id.id, remote_id.id, sizeof(tmp_id.id));
				sess.write_cas(tmp_id,
					std::bind(&update_indexes_functor::convert_index_table<insert_data>,
						shared_from_this(),
						previous_data[remote_id],
						std::placeholders::_1),
				0).connect(std::bind(&update_indexes_functor::on_index_table_reverted,
					shared_from_this(),
					std::placeholders::_2));
			}
		} else {
			handler(std::exception_ptr());
			return;
		}
	}

	/*!
	 * Updating of certain index table for \a id is finished with error \a err
	 */
	template <update_index_action action>
	void on_index_table_updated(const dnet_raw_id &id, const error_info &err)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++finished;

		if (err) {
			try {
				err.throw_error();
			} catch (...) {
				exception = std::current_exception();
			}
		} else {
			if (action == insert_data) {
				success_inserted_ids.push_back(id);
			} else {
				success_removed_ids.push_back(id);
			}
		}
		on_index_table_update_finished();
	}

	/*!
	 * Replace object's indexes cache by new table.
	 * Remembers current object's indexes to local variable remote_indexes
	 */
	data_pointer convert_object_indexes(const data_pointer &data)
	{
		if (data.empty()) {
			remote_indexes.indexes.clear();
		} else {
			indexes_unpack(data, &remote_indexes, "main_functor");
		}

		return data_pointer::from_raw(const_cast<char *>(buffer.data()), buffer.size());
	}

	/*!
	 * Handle result of object indexes' table update
	 */
	void on_object_indexes_updated(const sync_write_result &, const error_info &err)
	{
		// If there was an error - notify user about this.
		// At this state there were no changes at the storage yet.
		if (err) {
			try {
				err.throw_error();
			} catch (...) {
				handler(std::current_exception());
			}
			return;
		}

		try {
			// We "insert" items also to update their data
			std::set_difference(indexes.indexes.begin(), indexes.indexes.end(),
				remote_indexes.indexes.begin(), remote_indexes.indexes.end(),
				std::back_inserter(inserted_ids), dnet_raw_id_less_than<>());
			// Remove only absolutely another items
			std::set_difference(remote_indexes.indexes.begin(), remote_indexes.indexes.end(),
				indexes.indexes.begin(), indexes.indexes.end(),
				std::back_inserter(removed_ids), dnet_raw_id_less_than<skip_data>());

			if (inserted_ids.empty() && removed_ids.empty()) {
				handler(std::exception_ptr());
				return;
			}

			dnet_id tmp_id;
			tmp_id.group_id = 0;
			tmp_id.type = 0;

			for (size_t i = 0; i < inserted_ids.size(); ++i) {
				memcpy(tmp_id.id, inserted_ids[i].index.id, sizeof(tmp_id.id));
				sess.write_cas(tmp_id,
					std::bind(&update_indexes_functor::convert_index_table<insert_data>,
						shared_from_this(),
						inserted_ids[i].data,
						std::placeholders::_1),
					0).connect(std::bind(&update_indexes_functor::on_index_table_updated<insert_data>,
						shared_from_this(),
						inserted_ids[i].index,
						std::placeholders::_2));
			}

			for (size_t i = 0; i < removed_ids.size(); ++i) {
				memcpy(tmp_id.id, removed_ids[i].index.id, sizeof(tmp_id.id));
				sess.write_cas(tmp_id,
					std::bind(&update_indexes_functor::convert_index_table<remove_data>,
						shared_from_this(),
						removed_ids[i].data,
						std::placeholders::_1),
					0).connect(std::bind(&update_indexes_functor::on_index_table_updated<remove_data>,
						shared_from_this(),
						removed_ids[i].index,
						std::placeholders::_2));
			}
		} catch (...) {
			handler(std::current_exception());
			return;
		}
	}

	void start()
	{
		sess.write_cas(id, bind_method(shared_from_this(), &update_indexes_functor::convert_object_indexes), 0)
			.connect(bind_method(shared_from_this(), &update_indexes_functor::on_object_indexes_updated));
	}
};

// Update \a indexes for \a request_id
// Result is pushed to \a handler
void session::update_indexes(const std::function<void (const update_indexes_result &)> &handler,
	const key &request_id, const std::vector<index_entry> &indexes)
{
	transform(request_id);

	auto functor = std::make_shared<update_indexes_functor>(
		*this, handler, request_id, indexes, indexes_generate_id(*this, request_id.id()));
	functor->start();
}

void session::update_indexes(const key &request_id, const std::vector<index_entry> &indexes)
{
	transform(request_id);

	waiter<std::exception_ptr> w;
	update_indexes(w.handler(), request_id, indexes);
	w.result();
}

void session::update_indexes(const key &id, const std::vector<std::string> &indexes, const std::vector<data_pointer> &datas)
{
	if (datas.size() != indexes.size())
		throw_error(-EINVAL, id, "session::update_indexes: indexes and datas sizes mismtach");
	dnet_id tmp;
	std::vector<index_entry> raw_indexes;
	raw_indexes.resize(indexes.size());

	for (size_t i = 0; i < indexes.size(); ++i) {
		transform(indexes[i], tmp);
		memcpy(raw_indexes[i].index.id, tmp.id, sizeof(tmp.id));
		raw_indexes[i].data = datas[i];
	}

	update_indexes(id, raw_indexes);
}

struct find_indexes_handler
{
	std::function<void (const find_indexes_result &)> handler;
	size_t ios_size;

	void operator() (const sync_read_result &bulk_result, const error_info &err)
	{
		std::vector<find_indexes_result_entry> result;

		if (err == -ENOENT) {
			handler(result);
			return;
		} else if (err) {
			try {
				err.throw_error();
			} catch (...) {
				handler(std::current_exception());
			}
			return;
		}

		if (bulk_result.size() != ios_size) {
			handler(result);
			return;
		}

		try {
			dnet_indexes tmp;
			indexes_unpack(bulk_result[0].file(), &tmp, "find_indexes_handler1");
			result.resize(tmp.indexes.size());
			for (size_t i = 0; i < tmp.indexes.size(); ++i) {
				find_indexes_result_entry &entry = result[i];
				entry.id = tmp.indexes[i].index;
				entry.indexes.push_back(std::make_pair(
					reinterpret_cast<dnet_raw_id&>(bulk_result[0].command()->id),
					tmp.indexes[i].data));
			}

			for (size_t i = 1; i < bulk_result.size() && !result.empty(); ++i) {
				auto raw = reinterpret_cast<dnet_raw_id&>(bulk_result[i].command()->id);
				tmp.indexes.resize(0);
				indexes_unpack(bulk_result[i].file(), &tmp, "find_indexes_handler2");
				auto it = std::set_intersection(result.begin(), result.end(),
					tmp.indexes.begin(), tmp.indexes.end(),
					result.begin(), dnet_raw_id_less_than<skip_data>());
				result.resize(it - result.begin());
				std::set_intersection(tmp.indexes.begin(), tmp.indexes.end(),
					result.begin(), result.end(),
					tmp.indexes.begin(), dnet_raw_id_less_than<skip_data>());
				auto jt = tmp.indexes.begin();
				for (auto kt = result.begin(); kt != result.end(); ++kt, ++jt) {
					kt->indexes.push_back(std::make_pair(raw, jt->data));
				}
			}

			try {
				handler(result);
			} catch (...) {
			}
		} catch (...) {
			handler(std::current_exception());
			return;
		}
	}
};

void session::find_indexes(const std::function<void (const find_indexes_result &)> &handler, const std::vector<dnet_raw_id> &indexes)
{
	if (indexes.size() == 0) {
		std::vector<find_indexes_result_entry> results;
		handler(results);
		return;
	}

	std::vector<dnet_io_attr> ios;
	struct dnet_io_attr io;
	memset(&io, 0, sizeof(io));

	for (size_t i = 0; i < indexes.size(); ++i) {
		memcpy(io.id, indexes[i].id, sizeof(dnet_raw_id));
		ios.push_back(io);
	}

	find_indexes_handler functor = { handler, ios.size() };
	bulk_read(ios).connect(functor);
}

find_indexes_result session::find_indexes(const std::vector<dnet_raw_id> &indexes)
{
	waiter<find_indexes_result> w;
	find_indexes(w.handler(), indexes);
	return w.result();
}

find_indexes_result session::find_indexes(const std::vector<std::string> &indexes)
{
	dnet_id tmp;
	std::vector<dnet_raw_id> raw_indexes;
	raw_indexes.resize(indexes.size());

	for (size_t i = 0; i < indexes.size(); ++i) {
		transform(indexes[i], tmp);
		memcpy(raw_indexes[i].id, tmp.id, sizeof(tmp.id));
	}

	return find_indexes(raw_indexes);
}

struct check_indexes_handler
{
	std::function<void (const check_indexes_result &)> handler;

	void operator() (const sync_read_result &read_result, const error_info &err)
	{
		if (err) {
			try {
				err.throw_error();
			} catch (...) {
				handler(std::current_exception());
			}
			return;
		}

		try {
			dnet_indexes result;
			indexes_unpack(read_result[0].file(), &result, "check_indexes_handler");

			try {
				handler(result.indexes);
			} catch (...) {
			}
		} catch (...) {
			handler(std::current_exception());
			return;
		}
	}
};

void session::check_indexes(const std::function<void (const check_indexes_result &)> &handler, const key &request_id)
{
	dnet_id id = indexes_generate_id(*this, request_id.id());

	check_indexes_handler functor = { handler };
	read_latest(id, 0, 0).connect(functor);
}

check_indexes_result session::check_indexes(const key &id)
{
	waiter<check_indexes_result> w;
	check_indexes(w.handler(), id);
	return w.result();
}

} } // ioremap::elliptics
