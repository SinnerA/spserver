/*
 * Copyright 2007 Stephen Liu
 * For license terms, see the file COPYING along with this library.
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "speventcb.hpp"
#include "spsession.hpp"
#include "spexecutor.hpp"
#include "spresponse.hpp"
#include "sphandler.hpp"
#include "spbuffer.hpp"
#include "spmsgdecoder.hpp"
#include "sputils.hpp"
#include "sprequest.hpp"
#include "spmsgblock.hpp"

#include "config.h"   // from libevent, for event.h
#include "event_msgqueue.h"
#include "event.h"

SP_EventArg :: SP_EventArg( int timeout )
{
	mEventBase = (struct event_base*)event_init();

	mResponseQueue = msgqueue_new( mEventBase, 0,
			SP_EventCallback::onResponse, this );

	mInputResultQueue = new SP_BlockingQueue();

	mOutputResultQueue = new SP_BlockingQueue();

	mSessionManager = new SP_SessionManager();

	mTimeout = timeout;
}

SP_EventArg :: ~SP_EventArg()
{
	delete mInputResultQueue;
	delete mOutputResultQueue;

	delete mSessionManager;

	//msgqueue_destroy( (struct event_msgqueue*)mResponseQueue );
	//event_base_free( mEventBase );
}

struct event_base * SP_EventArg :: getEventBase() const
{
	return mEventBase;
}

void * SP_EventArg :: getResponseQueue() const
{
	return mResponseQueue;
}

SP_BlockingQueue * SP_EventArg :: getInputResultQueue() const
{
	return mInputResultQueue;
}

SP_BlockingQueue * SP_EventArg :: getOutputResultQueue() const
{
	return mOutputResultQueue;
}

SP_SessionManager * SP_EventArg :: getSessionManager() const
{
	return mSessionManager;
}

void SP_EventArg :: setTimeout( int timeout )
{
	mTimeout = timeout;
}

int SP_EventArg :: getTimeout() const
{
	return mTimeout;
}

//-------------------------------------------------------------------

void SP_EventCallback :: onAccept( int fd, short events, void * arg )
{
	int clientFD;
	struct sockaddr_in clientAddr;
	socklen_t clientLen = sizeof( clientAddr );

	SP_AcceptArg_t * acceptArg = (SP_AcceptArg_t*)arg;
	SP_EventArg * eventArg = acceptArg->mEventArg;

	clientFD = accept( fd, (struct sockaddr *)&clientAddr, &clientLen );
	if( -1 == clientFD ) {
		syslog( LOG_WARNING, "accept failed" );
		return;
	}

	if( SP_EventHelper::setNonblock( clientFD ) < 0 ) {
		syslog( LOG_WARNING, "failed to set client socket non-blocking" );
	}

	SP_Sid_t sid;
	sid.mKey = clientFD;
	eventArg->getSessionManager()->get( sid.mKey, &sid.mSeq );

	SP_Session * session = new SP_Session( sid );

	char clientIP[ 32 ] = { 0 };
	SP_EventHelper::inetNtoa( &( clientAddr.sin_addr ), clientIP, sizeof( clientIP ) );
	session->getRequest()->setClientIP( clientIP );

	if( NULL != session ) {
		eventArg->getSessionManager()->put( sid.mKey, session, &sid.mSeq );

		session->setHandler( acceptArg->mHandlerFactory->create() );
		session->setArg( eventArg );

		event_set( session->getReadEvent(), clientFD, EV_READ, onRead, session );
		event_set( session->getWriteEvent(), clientFD, EV_WRITE, onWrite, session );

		addEvent( session, EV_WRITE, clientFD );
		addEvent( session, EV_READ, clientFD );

		if( eventArg->getSessionManager()->getCount() > acceptArg->mMaxConnections
				|| eventArg->getInputResultQueue()->getLength() >= acceptArg->mReqQueueSize ) {
			syslog( LOG_WARNING, "System busy, session.count %d [%d], queue.length %d [%d]",
				eventArg->getSessionManager()->getCount(), acceptArg->mMaxConnections,
				eventArg->getInputResultQueue()->getLength(), acceptArg->mReqQueueSize );

			SP_Message * msg = new SP_Message();
			msg->getMsg()->append( acceptArg->mRefusedMsg );
			msg->getMsg()->append( "\r\n" );
			session->getOutList()->append( msg );

			session->setStatus( SP_Session::eExit );
		} else {
			SP_EventHelper::doStart( session );
		}
	} else {
		close( clientFD );
		syslog( LOG_WARNING, "Out of memory, cannot allocate session object!" );
	}
}

void SP_EventCallback :: onRead( int fd, short events, void * arg )
{
	SP_Session * session = (SP_Session*)arg;

	SP_Sid_t sid = session->getSid();

	if( EV_READ & events ) {
		int len = session->getInBuffer()->read( fd );

		if( len > 0 ) {
			if( 0 == session->getRunning() ) {
				SP_MsgDecoder * decoder = session->getRequest()->getMsgDecoder();
				if( SP_MsgDecoder::eOK == decoder->decode( session->getInBuffer() ) ) {
					SP_EventHelper::doWork( session );
				}
			}
			addEvent( session, EV_READ, -1 );
		} else {
			if( 0 == session->getRunning() ) {
				syslog( LOG_NOTICE, "session(%d.%d) read error", sid.mKey, sid.mSeq );
				SP_EventHelper::doError( session );
			} else {
				addEvent( session, EV_READ, -1 );
				syslog( LOG_NOTICE, "session(%d.%d) busy, process session error later",
						sid.mKey, sid.mSeq );
			}
		}
	} else {
		if( 0 == session->getRunning() ) {
			SP_EventHelper::doTimeout( session );
		} else {
			addEvent( session, EV_READ, -1 );
			syslog( LOG_NOTICE, "session(%d.%d) busy, process session timeout later",
					sid.mKey, sid.mSeq );
		}
	}
}

void SP_EventCallback :: onWrite( int fd, short events, void * arg )
{
	SP_Session * session = (SP_Session*)arg;

	SP_Handler * handler = session->getHandler();
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	session->setWriting( 0 );

	SP_Sid_t sid = session->getSid();

	if( EV_WRITE & events ) {
		int ret = 0;

		if( session->getOutList()->getCount() > 0 ) {
			int len = SP_EventHelper::transmit( session, fd );

			if( len > 0 ) {
				if( session->getOutList()->getCount() > 0 ) {
					// left for next write event
					addEvent( session, EV_WRITE, -1 );
				}
			} else {
				if( EINTR != errno && EAGAIN != errno ) {
					ret = -1;
					if( 0 == session->getRunning() ) {
						syslog( LOG_NOTICE, "session(%d.%d) write error", sid.mKey, sid.mSeq );
						SP_EventHelper::doError( session );
					} else {
						addEvent( session, EV_WRITE, -1 );
						syslog( LOG_NOTICE, "session(%d.%d) busy, process session error later, errno [%d]",
								sid.mKey, sid.mSeq, errno );
					}
				}
			}
		}

		if( 0 == ret && session->getOutList()->getCount() <= 0 ) {
			if( SP_Session::eExit == session->getStatus() ) {
				ret = -1;
				if( 0 == session->getRunning() ) {
					syslog( LOG_NOTICE, "session(%d.%d) close, exit", sid.mKey, sid.mSeq );

					eventArg->getSessionManager()->remove( fd );
					event_del( session->getReadEvent() );
					handler->close();
					close( fd );
					delete session;
				} else {
					addEvent( session, EV_WRITE, -1 );
					syslog( LOG_NOTICE, "session(%d.%d) busy, terminate session later",
							sid.mKey, sid.mSeq );
				}
			}
		}

		if( 0 == ret ) {
			if( 0 == session->getRunning() ) {
				SP_MsgDecoder * decoder = session->getRequest()->getMsgDecoder();
				if( SP_MsgDecoder::eOK == decoder->decode( session->getInBuffer() ) ) {
					SP_EventHelper::doWork( session );
				}
			} else {
				// If this session is running, then onResponse will add write event for this session.
				// So no need to add write event here.
			}
		}
	} else {
		if( 0 == session->getRunning() ) {
			SP_EventHelper::doTimeout( session );
		} else {
			addEvent( session, EV_WRITE, -1 );
			syslog( LOG_NOTICE, "session(%d.%d) busy, process session timeout later",
					sid.mKey, sid.mSeq );
		}
	}
}

void SP_EventCallback :: onResponse( void * queueData, void * arg )
{
	SP_Response * response = (SP_Response*)queueData;
	SP_EventArg * eventArg = (SP_EventArg*)arg;
	SP_SessionManager * manager = eventArg->getSessionManager();

	SP_Sid_t fromSid = response->getFromSid();
	u_int16_t seq = 0;

	if( ! SP_EventHelper::isSystemSid( &fromSid ) ) {
		SP_Session * session = manager->get( fromSid.mKey, &seq );
		if( seq == fromSid.mSeq && NULL != session ) {
			if( SP_Session::eWouldExit == session->getStatus() ) {
				session->setStatus( SP_Session::eExit );
			}

			if( SP_Session::eNormal != session->getStatus() ) {
				event_del( session->getReadEvent() );
			}

			// always add a write event for sender, 
			// so the pending input can be processed in onWrite
			addEvent( session, EV_WRITE, -1 );
		} else {
			syslog( LOG_WARNING, "session(%d.%d) invalid, unknown FROM",
					fromSid.mKey, fromSid.mSeq );
		}
	}

	for( SP_Message * msg = response->takeMessage();
			NULL != msg; msg = response->takeMessage() ) {

		SP_SidList * sidList = msg->getToList();

		if( msg->getTotalSize() > 0 ) {
			for( int i = sidList->getCount() - 1; i >= 0; i-- ) {
				SP_Sid_t sid = sidList->get( i );
				SP_Session * session = manager->get( sid.mKey, &seq );
				if( seq == sid.mSeq && NULL != session ) {
					if( 0 != memcmp( &fromSid, &sid, sizeof( sid ) )
							&& SP_Session::eExit == session->getStatus() ) {
						sidList->take( i );
						msg->getFailure()->add( sid );
						syslog( LOG_WARNING, "session(%d.%d) would exit, invalid TO", sid.mKey, sid.mSeq );
					} else {
						session->getOutList()->append( msg );
						addEvent( session, EV_WRITE, -1 );
					}
				} else {
					sidList->take( i );
					msg->getFailure()->add( sid );
					syslog( LOG_WARNING, "session(%d.%d) invalid, unknown TO", sid.mKey, sid.mSeq );
				}
			}
		} else {
			for( ; sidList->getCount() > 0; ) {
				msg->getFailure()->add( sidList->take( SP_ArrayList::LAST_INDEX ) );
			}
		}

		if( msg->getToList()->getCount() <= 0 ) {
			SP_EventHelper::doCompletion( eventArg, msg );
		}
	}

	delete response;
}

void SP_EventCallback :: addEvent( SP_Session * session, short events, int fd )
{
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	if( ( events & EV_WRITE ) && 0 == session->getWriting() ) {
		session->setWriting( 1 );

		if( fd < 0 ) fd = EVENT_FD( session->getWriteEvent() );

		event_set( session->getWriteEvent(), fd, events, onWrite, session );
		event_base_set( eventArg->getEventBase(), session->getWriteEvent() );

		struct timeval timeout;
		memset( &timeout, 0, sizeof( timeout ) );
		timeout.tv_sec = eventArg->getTimeout();
		event_add( session->getWriteEvent(), &timeout );
	}

	if( events & EV_READ ) {
		if( fd < 0 ) fd = EVENT_FD( session->getWriteEvent() );

		event_set( session->getReadEvent(), fd, events, onRead, session );
		event_base_set( eventArg->getEventBase(), session->getReadEvent() );

		struct timeval timeout;
		memset( &timeout, 0, sizeof( timeout ) );
		timeout.tv_sec = eventArg->getTimeout();
		event_add( session->getReadEvent(), &timeout );
	}
}

//-------------------------------------------------------------------

int SP_EventHelper :: setNonblock( int fd )
{
	int flags;

	flags = fcntl( fd, F_GETFL);
	if( flags < 0 ) return flags;

	flags |= O_NONBLOCK;
	if( fcntl( fd, F_SETFL, flags ) < 0 ) return -1;

	return 0;
}

void SP_EventHelper :: inetNtoa( in_addr * addr, char * ip, int size )
{
#if defined (linux) || defined (__sgi) || defined (__hpux) || defined (__FreeBSD__)
	const unsigned char *p = ( const unsigned char *) addr;
	snprintf( ip, size, "%i.%i.%i.%i", p[0], p[1], p[2], p[3] );
#else
	snprintf( ip, size, "%i.%i.%i.%i", addr->s_net, addr->s_host, addr->s_lh, addr->s_impno );
#endif
}

int SP_EventHelper :: isSystemSid( SP_Sid_t * sid )
{
	return sid->mKey == SP_Sid_t::eTimerKey && sid->mSeq == SP_Sid_t::eTimerSeq;
}

int SP_EventHelper :: tcpListen( const char * ip, int port, int * fd, int blocking )
{
	int ret = 0;

	int listenFd = socket( AF_INET, SOCK_STREAM, 0 );
	if( listenFd < 0 ) {
		syslog( LOG_WARNING, "listen failed" );
		ret = -1;
	}

	if( 0 == ret && 0 == blocking ) {
		if( setNonblock( listenFd ) < 0 ) {
			syslog( LOG_WARNING, "failed to set socket to non-blocking" );
			ret = -1;
		}
	}

	if( 0 == ret ) {
		int flags = 1;
		if( setsockopt( listenFd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof( flags ) ) < 0 ) {
			syslog( LOG_WARNING, "failed to set setsock to reuseaddr" );
			ret = -1;
		}
		if( setsockopt( listenFd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags) ) < 0 ) {
			syslog( LOG_WARNING, "failed to set socket to nodelay" );
			ret = -1;
		}
	}

	struct sockaddr_in addr;

	if( 0 == ret ) {
		memset( &addr, 0, sizeof( addr ) );
		addr.sin_family = AF_INET;
		addr.sin_port = htons( port );

		addr.sin_addr.s_addr = INADDR_ANY;
		if( '\0' != *ip ) {
			if( 0 != inet_aton( ip, &addr.sin_addr ) ) {
				syslog( LOG_WARNING, "failed to convert %s to inet_addr", ip );
				ret = -1;
			}
		}
	}

	if( 0 == ret ) {
		if( bind( listenFd, (struct sockaddr*)&addr, sizeof( addr ) ) < 0 ) {
			syslog( LOG_WARNING, "bind failed" );
			ret = -1;
		}
	}

	if( 0 == ret ) {
		if( ::listen( listenFd, 5 ) < 0 ) {
			syslog( LOG_WARNING, "listen failed" );
			ret = -1;
		}
	}

	if( 0 != ret && listenFd >= 0 ) close( listenFd );

	if( 0 == ret ) {
		* fd = listenFd;
		syslog( LOG_NOTICE, "Listen on port [%d]", port );
	}

	return ret;
}

void SP_EventHelper :: doWork( SP_Session * session )
{
	if( SP_Session::eNormal == session->getStatus() ) {
		session->setRunning( 1 );
		SP_EventArg * eventArg = (SP_EventArg*)session->getArg();
		eventArg->getInputResultQueue()->push( new SP_SimpleTask( worker, session, 1 ) );
	} else {
		SP_Sid_t sid = session->getSid();

		char buffer[ 16 ] = { 0 };
		session->getInBuffer()->take( buffer, sizeof( buffer ) );
		syslog( LOG_WARNING, "session(%d.%d) status is %d, ignore [%s...] (%dB)",
			sid.mKey, sid.mSeq, session->getStatus(), buffer, session->getInBuffer()->getSize() );
		session->getInBuffer()->reset();
	}
}

void SP_EventHelper :: worker( void * arg )
{
	SP_Session * session = (SP_Session*)arg;
	SP_Handler * handler = session->getHandler();
	SP_EventArg * eventArg = (SP_EventArg *)session->getArg();

	SP_Response * response = new SP_Response( session->getSid() );
	if( 0 != handler->handle( session->getRequest(), response ) ) {
		session->setStatus( SP_Session::eWouldExit );
	}

	session->setRunning( 0 );

	msgqueue_push( (struct event_msgqueue*)eventArg->getResponseQueue(), response );
}

void SP_EventHelper :: doError( SP_Session * session )
{
	SP_EventArg * eventArg = (SP_EventArg *)session->getArg();

	event_del( session->getWriteEvent() );
	event_del( session->getReadEvent() );

	SP_Sid_t sid = session->getSid();

	SP_ArrayList * outList = session->getOutList();
	for( ; outList->getCount() > 0; ) {
		SP_Message * msg = ( SP_Message * ) outList->takeItem( SP_ArrayList::LAST_INDEX );

		int index = msg->getToList()->find( sid );
		if( index >= 0 ) msg->getToList()->take( index );
		msg->getFailure()->add( sid );

		if( msg->getToList()->getCount() <= 0 ) {
			doCompletion( eventArg, msg );
		}
	}

	// remove session from SessionManager, onResponse will ignore this session
	eventArg->getSessionManager()->remove( sid.mKey );

	eventArg->getInputResultQueue()->push( new SP_SimpleTask( error, session, 1 ) );
}

void SP_EventHelper :: error( void * arg )
{
	SP_Session * session = ( SP_Session * )arg;
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	SP_Sid_t sid = session->getSid();

	SP_Response * response = new SP_Response( sid );
	session->getHandler()->error( response );

	msgqueue_push( (struct event_msgqueue*)eventArg->getResponseQueue(), response );

	// onResponse will ignore this session, so it's safe to destroy session here
	session->getHandler()->close();
	close( EVENT_FD( session->getWriteEvent() ) );
	delete session;
	syslog( LOG_WARNING, "session(%d.%d) error, exit", sid.mKey, sid.mSeq );
}

void SP_EventHelper :: doTimeout( SP_Session * session )
{
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	event_del( session->getWriteEvent() );
	event_del( session->getReadEvent() );

	SP_Sid_t sid = session->getSid();

	SP_ArrayList * outList = session->getOutList();
	for( ; outList->getCount() > 0; ) {
		SP_Message * msg = ( SP_Message * ) outList->takeItem( SP_ArrayList::LAST_INDEX );

		int index = msg->getToList()->find( sid );
		if( index >= 0 ) msg->getToList()->take( index );
		msg->getFailure()->add( sid );

		if( msg->getToList()->getCount() <= 0 ) {
			doCompletion( eventArg, msg );
		}
	}

	// remove session from SessionManager, onResponse will ignore this session
	eventArg->getSessionManager()->remove( sid.mKey );

	eventArg->getInputResultQueue()->push( new SP_SimpleTask( timeout, session, 1 ) );
}

void SP_EventHelper :: timeout( void * arg )
{
	SP_Session * session = ( SP_Session * )arg;
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	SP_Sid_t sid = session->getSid();

	SP_Response * response = new SP_Response( sid );
	session->getHandler()->timeout( response );
	msgqueue_push( (struct event_msgqueue*)eventArg->getResponseQueue(), response );

	// onResponse will ignore this session, so it's safe to destroy session here
	session->getHandler()->close();
	close( EVENT_FD( session->getWriteEvent() ) );
	delete session;
	syslog( LOG_WARNING, "session(%d.%d) timeout, exit", sid.mKey, sid.mSeq );
}


void SP_EventHelper :: doStart( SP_Session * session )
{
	session->setRunning( 1 );
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();
	eventArg->getInputResultQueue()->push( new SP_SimpleTask( start, session, 1 ) );
}

void SP_EventHelper :: start( void * arg )
{
	SP_Session * session = ( SP_Session * )arg;
	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	SP_Response * response = new SP_Response( session->getSid() );

	if( 0 != session->getHandler()->start( session->getRequest(), response ) ) {
		session->setStatus( SP_Session::eWouldExit );
	}

	session->setRunning( 0 );

	msgqueue_push( (struct event_msgqueue*)eventArg->getResponseQueue(), response );
}

void SP_EventHelper :: doCompletion( SP_EventArg * eventArg, SP_Message * msg )
{
	eventArg->getOutputResultQueue()->push( msg );
}

int SP_EventHelper :: transmit( SP_Session * session, int fd )
{
#ifdef IOV_MAX
	const static int SP_MAX_IOV = IOV_MAX;
#else
	const static int SP_MAX_IOV = 8;
#endif

	SP_EventArg * eventArg = (SP_EventArg*)session->getArg();

	SP_ArrayList * outList = session->getOutList();
	size_t outOffset = session->getOutOffset();

	struct iovec iovArray[ SP_MAX_IOV ];
	memset( iovArray, 0, sizeof( iovArray ) );

	int iovSize = 0;

	for( int i = 0; i < outList->getCount() && iovSize < SP_MAX_IOV; i++ ) {
		SP_Message * msg = (SP_Message*)outList->getItem( i );

		if( outOffset >= msg->getMsg()->getSize() ) {
			outOffset -= msg->getMsg()->getSize();
		} else {
			iovArray[ iovSize ].iov_base = (char*)msg->getMsg()->getBuffer() + outOffset;
			iovArray[ iovSize++ ].iov_len = msg->getMsg()->getSize() - outOffset;
			outOffset = 0;
		}

		SP_MsgBlockList * blockList = msg->getFollowBlockList();
		for( int j = 0; j < blockList->getCount() && iovSize < SP_MAX_IOV; j++ ) {
			SP_MsgBlock * block = (SP_MsgBlock*)blockList->getItem( j );

			if( outOffset >= block->getSize() ) {
				outOffset -= block->getSize();
			} else {
				iovArray[ iovSize ].iov_base = (char*)block->getData() + outOffset;
				iovArray[ iovSize++ ].iov_len = block->getSize() - outOffset;
				outOffset = 0;
			}
		}
	}

	int len = writev( fd, iovArray, iovSize );

	if( len > 0 ) {
		outOffset = session->getOutOffset() + len;

		for( ; outList->getCount() > 0; ) {
			SP_Message * msg = (SP_Message*)outList->getItem( 0 );
			if( outOffset >= msg->getTotalSize() ) {
				msg = (SP_Message*)outList->takeItem( 0 );
				outOffset = outOffset - msg->getTotalSize();

				int index = msg->getToList()->find( session->getSid() );
				if( index >= 0 ) msg->getToList()->take( index );
				msg->getSuccess()->add( session->getSid() );

				if( msg->getToList()->getCount() <= 0 ) {
					doCompletion( eventArg, msg );
				}
			} else {
				break;
			}
		}

		session->setOutOffset( outOffset );
	}

	if( len > 0 && outList->getCount() > 0 ) transmit( session, fd );

	return len;
}

