/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __EDEF_H
#define __EDEF_H

#include <errno.h>
#include <stdint.h>

#include "elliptics/packet.h"
#include "elliptics/interface.h"

#include <iostream>
#include <fstream>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

class elliptics_log {
	public:
		elliptics_log(const uint32_t mask = DNET_LOG_ERROR | DNET_LOG_INFO) {
			ll.log_mask = mask;
			ll.log = elliptics_log::logger;
			ll.log_private = this;
		};
		virtual ~elliptics_log() {};

		virtual void 		log(const uint32_t mask, const char *msg) = 0;

		/*
		 * Clone is used instead of 'virtual' copy constructor, since we have to
		 * hold a reference to object outside of our scope, namely python created
		 * logger. This is also a reason we return 'unsigned long' instead of
		 * 'elliptics_log *' - python does not have pointer.
		 */
		virtual unsigned long	clone(void) = 0;

		static void		logger(void *priv, const uint32_t mask, const char *msg);
		uint32_t		get_log_mask(void) { return ll.log_mask; };
		struct dnet_log		*get_dnet_log(void) { return &ll; };
	protected:
		struct dnet_log		ll;
};

class elliptics_log_file : public elliptics_log {
	public:
		elliptics_log_file(const char *file, const uint32_t mask = DNET_LOG_ERROR | DNET_LOG_INFO);
		virtual ~elliptics_log_file();

		virtual unsigned long	clone(void);
		virtual void 		log(const uint32_t mask, const char *msg);

		std::string		*file;
	private:
		/*
		 * Oh shi, I put pointer here to avoid boost::python compiler issues,
		 * when it tries to copy stream, which is not allowed
		 */
		std::ofstream		*stream;
};

class elliptics_callback {
	public:
		elliptics_callback();
		virtual ~elliptics_callback();

		virtual int callback(void);

		bool last(void) {
			return (!cmd || !(cmd->flags & DNET_FLAGS_MORE));
		};

		int status(void) {
			int err = -EINVAL;
			if (cmd)
				err = cmd->status;

			return err;
		};

		static int elliptics_complete_callback(struct dnet_net_state *st, struct dnet_cmd *cmd, struct dnet_attr *a, void *priv) {
			elliptics_callback *c = reinterpret_cast<elliptics_callback *>(priv);

			c->state = st;
			c->cmd = cmd;
			c->attr = a;

			int ret = c->callback();

			c->state = NULL;
			c->cmd = NULL;
			c->attr = NULL;

			return ret;
		};

		std::string wait(int completed = 1);

	protected:
		struct dnet_net_state	*state;
		struct dnet_cmd		*cmd;
		struct dnet_attr	*attr;

		std::string		data;
		pthread_cond_t		wait_cond;
		pthread_mutex_t		lock;
		int			complete;
};

class elliptics_node {
	public:
		/* we shold use elliptics_log and proper copy constructor here, but not this time */
		elliptics_node(elliptics_log &l);
		elliptics_node(elliptics_log &l, struct dnet_config &cfg);
		virtual ~elliptics_node();

		void			transform(const std::string &data, struct dnet_id &id);

		void			add_groups(std::vector<int> &groups);
		std::vector<int>	&get_groups() {return groups;};

		void			add_remote(const char *addr, const int port, const int family = AF_INET);

		void			read_file(struct dnet_id &id, char *dst_file, uint64_t offset, uint64_t size);
		void			read_file(std::string &remote, char *dst_file, uint64_t offset, uint64_t size);

		void			read_data(struct dnet_id &id, uint64_t offset, uint64_t size, elliptics_callback &c);
		void			read_data(std::string &remote, uint64_t offset, uint64_t size, elliptics_callback &c);

		void 			write_file(struct dnet_id &id, char *src_file, uint64_t local_offset, uint64_t offset, uint64_t size,
							unsigned int aflags = 0, unsigned int ioflags = 0);
		void			write_file(std::string &remote, char *src_file, uint64_t local_offset,
							uint64_t offset, uint64_t size,
							unsigned int aflags = 0, unsigned int ioflags = 0);

		int			write_data(struct dnet_id &id, std::string &str, elliptics_callback &c,
							unsigned int aflags = 0, unsigned int ioflags = 0);
		int			write_data(std::string &remote, std::string &str, elliptics_callback &c,
							unsigned int aflags = 0, unsigned int ioflags = 0);

		std::string		read_data_wait(struct dnet_id &id, uint64_t size);
		std::string		read_data_wait(std::string &remote, uint64_t size);

		int			write_data_wait(struct dnet_id &id, std::string &str,
							unsigned int aflags = DNET_ATTR_DIRECT_TRANSACTION,
							unsigned int ioflags = DNET_IO_FLAGS_NO_HISTORY_UPDATE);
		int			write_data_wait(std::string &remote, std::string &str,
							unsigned int aflags = DNET_ATTR_DIRECT_TRANSACTION,
							unsigned int ioflags = DNET_IO_FLAGS_NO_HISTORY_UPDATE);

		std::string		lookup_addr(const std::string &remote, const int group_id);

		int			write_metadata(const struct dnet_id &id, const std::string &obj, const std::vector<int> &groups);

		void			lookup(const std::string &data, const elliptics_callback &c);
		void			lookup(const struct dnet_id &id, const elliptics_callback &c);
		std::string		lookup(const std::string &data);

		void 			remove(struct dnet_id &id);
		void			remove(const std::string &data);

		std::string		stat_log();

		int			state_num();

	private:
		int			write_data_ll(struct dnet_id *id, void *remote, unsigned int remote_len,
							void *data, unsigned int size, elliptics_callback &c,
							unsigned int aflags, unsigned int ioflags);
		struct dnet_node	*node;
		elliptics_log		*log;

		std::vector<int>	groups;
};

#endif /* __EDEF_H */
