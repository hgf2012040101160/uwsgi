#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

int uwsgi_enqueue_message(char *host, int port, uint8_t modifier1, uint8_t modifier2, char *message, int size, int timeout) {

	struct pollfd uwsgi_poll;
	struct sockaddr_in uws_addr;
        int cnt ;
        struct uwsgi_header uh;

	if (!timeout)
		timeout = 1;

	if (size > 0xFFFF) {
                fprintf(stderr,"invalid object (marshalled) size\n");
		return -1 ;
        }

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		perror("socket()");
		return -1 ;
	}

	memset(&uws_addr, 0, sizeof(struct sockaddr_in));
        uws_addr.sin_family = AF_INET;
        uws_addr.sin_port = htons(port);
        uws_addr.sin_addr.s_addr = inet_addr(host);

	uwsgi_poll.events = POLLIN ;

        if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &uws_addr, sizeof(struct sockaddr_in), timeout)) {
                perror("connect()");
                close(uwsgi_poll.fd);
		return -1 ;
        }

        uh.modifier1 = modifier1;
        uh.pktsize = (uint16_t) size ;
        uh.modifier2 = modifier2;

        cnt = write(uwsgi_poll.fd, &uh, 4) ;
        if (cnt != 4) {
                perror("write()");
                close(uwsgi_poll.fd);
                return -1;
        }

	cnt = write(uwsgi_poll.fd, message, size) ;
        if (cnt != size) {
                perror("write()");
                close(uwsgi_poll.fd);
                return -1;
        }

	return uwsgi_poll.fd;
}

PyObject *uwsgi_send_message(const char *host, int port, uint8_t modifier1, uint8_t modifier2, char *message, int size, int timeout) {

	struct pollfd uwsgi_mpoll ;
	struct sockaddr_in uws_addr;
	int cnt ;
	struct uwsgi_header uh;
	char buffer[0xFFFF];



	if (!timeout)
		timeout = 1;

	if (size > 0xFFFF) {
		fprintf(stderr,"invalid object (marshalled) size\n");
		Py_INCREF(Py_None);
                return Py_None;
	}

	uwsgi_mpoll.events = POLLIN ;

	uwsgi_mpoll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_mpoll.fd < 0) {
		perror("socket()");
		Py_INCREF(Py_None);
                return Py_None;
	}

	memset(&uws_addr, 0, sizeof(struct sockaddr_in));
	uws_addr.sin_family = AF_INET;
        uws_addr.sin_port = htons(port);
	uws_addr.sin_addr.s_addr = inet_addr(host);


	if (timed_connect(&uwsgi_mpoll, (const struct sockaddr *) &uws_addr, sizeof(struct sockaddr_in), timeout)) {
		perror("connect()");
		close(uwsgi_mpoll.fd);
		Py_INCREF(Py_None);
                return Py_None;
	}

	uh.modifier1 = modifier1;
	uh.pktsize = (uint16_t) size ;
	uh.modifier2 = modifier2;

	cnt = write(uwsgi_mpoll.fd, &uh, 4) ;
	if (cnt != 4) {
                perror("write()");      
                close(uwsgi_mpoll.fd);
                Py_INCREF(Py_None);
                return Py_None;
        }
	
	cnt = write(uwsgi_mpoll.fd, message, size) ;
	if (cnt != size) {
		perror("write()");	
		close(uwsgi_mpoll.fd);
		Py_INCREF(Py_None);
		return Py_None;
	}
	
	
	if (!uwsgi_parse_response(&uwsgi_mpoll, timeout, &uh, buffer)) {
		Py_INCREF(Py_None);
                return Py_None;
	}

	close(uwsgi_mpoll.fd);

	if (uh.modifier1 == UWSGI_MODIFIER_RESPONSE) {
		if (!uh.modifier2) {
			Py_INCREF(Py_None);
			return Py_None ;
		}
		else {
			Py_INCREF(Py_True);
			return Py_True ;
		}
	}
	
	return PyMarshal_ReadObjectFromString(buffer, uh.pktsize);	
}

int uwsgi_parse_response(struct pollfd * upoll, int timeout, struct uwsgi_header *uh, char *buffer) {
	int rlen, i;

	if (!timeout)
		timeout = 1;
	/* first 4 byte header */
                rlen = poll(upoll, 1, timeout*1000) ;
                if (rlen < 0) {
                        perror("poll()");
                        exit(1);
                }
                else if (rlen == 0) {
                        fprintf(stderr, "timeout. skip request\n");
                        close(upoll->fd);
                        return 0 ;
                }
                rlen = read(upoll->fd, uh, 4) ;
                if (rlen > 0 && rlen < 4) {
                        i = rlen ;
                        while(i < 4) {
                                rlen = poll(upoll, 1, timeout*1000) ;
                                if (rlen < 0) {
                                        perror("poll()");
                                        exit(1);
                                }
                                else if (rlen == 0) {
                                        fprintf(stderr, "timeout waiting for header. skip request.\n");
                                        close(upoll->fd);
                                        break ;
                                }
                                rlen = read(upoll->fd, (char *)(uh)+i, 4-i);
                                if (rlen <= 0) {
                                        fprintf(stderr, "broken header. skip request.\n");
                                        close(upoll->fd);
                                        break ;
                                }
                                i += rlen;
                        }
                        if (i < 4) {
                                return 0;
                        }
                }
                else if (rlen <= 0){
                        fprintf(stderr,"invalid request header size: %d...skip\n", rlen);
                        close(upoll->fd);
                        return 0;
                }
                /* big endian ? */
                #ifdef __BIG_ENDIAN__
                uh->pktsize = uwsgi_swap16(uh->pktsize);
                #endif

                /* check for max buffer size */
                if (uh->pktsize > uwsgi.buffer_size) {
                        fprintf(stderr,"invalid request block size: %d...skip\n", uh->pktsize);
                        close(upoll->fd);
			return 0;
                }

                //fprintf(stderr,"ready for reading %d bytes\n", wsgi_req.size);

                i = 0 ;
                while(i < uh->pktsize) {
                        rlen = poll(upoll, 1, timeout*1000) ;
                        if (rlen < 0) {
                                perror("poll()");
                                exit(1);
                        }
                        else if (rlen == 0) {
                                fprintf(stderr, "timeout. skip request. (expecting %d bytes, got %d)\n", uh->pktsize, i);
                                close(upoll->fd);
                                break ;
                        }
                        rlen = read(upoll->fd, buffer+i, uh->pktsize-i);
                        if (rlen <= 0) {
                                fprintf(stderr, "broken vars. skip request.\n");
                                close(upoll->fd);
                                break ;
                        }
                        i += rlen ;
                }


                if (i < uh->pktsize) {
                        return 0;
                }

		return 1;
}

int uwsgi_parse_vars(struct uwsgi_server *uwsgi, struct wsgi_request *wsgi_req) {

	char *buffer = uwsgi->buffer ;

	char *ptrbuf, *bufferend ;

	uint16_t strsize = 0;

	ptrbuf = buffer;
        bufferend = ptrbuf + wsgi_req->size;

        /* set an HTTP 500 status as default */
        wsgi_req->status = 500;

        while (ptrbuf < bufferend) {
                if (ptrbuf + 2 < bufferend) {
                        memcpy (&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                        strsize = uwsgi_swap16 (strsize);
#endif

			/* key cannot be null */
			if (!strsize) {
                        	fprintf (stderr, "uwsgi key cannot be null. skip this request.\n");
                                return -1;
			}
                        ptrbuf += 2;
                        if (ptrbuf + strsize < bufferend) {
                                // var key
                                uwsgi->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
                                uwsgi->hvec[wsgi_req->var_cnt].iov_len = strsize;
                                ptrbuf += strsize;
                                if (ptrbuf + 2 < bufferend) {
                                        memcpy (&strsize, ptrbuf, 2);
#ifdef __BIG_ENDIAN__
                                        strsize = uwsgi_swap16 (strsize);
#endif
                                        ptrbuf += 2;
                                        if (ptrbuf + strsize <= bufferend) {
                                                if (wsgi_req->var_cnt < uwsgi->vec_size - (4 + 1)) {
                                                        wsgi_req->var_cnt++;
                                                }
                                                else {
                                                        fprintf (stderr, "max vec size reached. skip this header.\n");
                                                        return -1;
                                                }
                                                // var value
                                                uwsgi->hvec[wsgi_req->var_cnt].iov_base = ptrbuf;
                                                uwsgi->hvec[wsgi_req->var_cnt].iov_len = strsize;
                                                if (wsgi_req->var_cnt < uwsgi->vec_size - (4 + 1)) {
                                                        wsgi_req->var_cnt++;
                                                }
                                                else {
                                                        fprintf (stderr, "max vec size reached. skip this header.\n");
                                                        return -1 ;
                                                }
                                                ptrbuf += strsize;
                                        }
                                        else {
                                                return -1;
                                        }
                                }
                                else {
                                        return -1;
                                }
                        }
                }
                else {
                        return -1;
                }
        }

	return 0 ;
}

int uwsgi_ping_node(int node, struct wsgi_request *wsgi_req) {


	struct pollfd uwsgi_poll ;

	struct uwsgi_cluster_node *ucn = &uwsgi.shared->nodes[node] ;

	if (ucn->name[0] == 0) {
		return 0 ;
	}

	if (ucn->status == UWSGI_NODE_OK) {
		return 0 ;
	}

	uwsgi_poll.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (uwsgi_poll.fd < 0) {
		perror("socket()");
		return -1 ;
	}

	if (timed_connect(&uwsgi_poll, (const struct sockaddr *) &ucn->ucn_addr, sizeof(struct sockaddr_in), uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT])) {
		close(uwsgi_poll.fd);
		return -1 ;
	}	

        wsgi_req->modifier = UWSGI_MODIFIER_PING ;
        wsgi_req->size = 0 ;
        wsgi_req->modifier_arg = 0 ;
        if (write(uwsgi_poll.fd, wsgi_req, 4) != 4) {
        	perror("write()");
		return -1;
	}

	uwsgi_poll.events = POLLIN ;
	if (!uwsgi_parse_response (&uwsgi_poll, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], (struct uwsgi_header *) wsgi_req, uwsgi.buffer)) {
		return -1;
	}

	return 0;
}
