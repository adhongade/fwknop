/* $Id$
 *****************************************************************************
 *
 * File:    pcap_capture.c
 *
 * Author:  Damien S. Stuart
 *
 * Purpose: The pcap capture routines for fwknopd.
 *
 * Copyright (C) 2009 Damien Stuart (dstuart@dstuart.org)
 *
 *  License (GNU Public License):
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *     USA
 *
 *****************************************************************************
*/
#include <pcap.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "fwknopd_common.h"
#include "pcap_capture.h"
#include "process_packet.h"
#include "incoming_spa.h"
#include "config_init.h"
#include "sig_handler.h"
#include "fw_util.h"
#include "log_msg.h"
#include "sig_handler.h"

/* The pcap capture routine.
*/
int
pcap_capture(fko_srv_options_t *opts)
{
#if HAVE_LIBPCAP
    pcap_t              *pcap;
    char                errstr[PCAP_ERRBUF_SIZE] = {0};
    struct bpf_program  fp;
    int                 res;
    int                 pcap_errcnt = 0;
    int                 pending_break = 0;
    int                 promisc = 1;
    int                 status;
    pid_t               child_pid;

    /* Set non-promiscuous mode only of the ENABLE_PCAP_PROMISC is
     * explicitly set to 'N'.
    */
    if(opts->config[CONF_ENABLE_PCAP_PROMISC] != NULL
      && opts->config[CONF_ENABLE_PCAP_PROMISC][0] == 'N')
        promisc = 0;

    pcap = pcap_open_live(
        opts->config[CONF_PCAP_INTF],
        atoi(opts->config[CONF_MAX_SNIFF_BYTES]),
        promisc, 500, errstr
    );

    if(pcap == NULL)
    {
        log_msg(LOG_ERR|LOG_STDERR, "* pcap_open_live error: %s\n", errstr);
        exit(EXIT_FAILURE);
    }

    /* We are only interested on seeing packets coming into the interface.
    */
    if (pcap_setdirection(pcap, PCAP_D_IN) < 0)
        log_msg(LOG_WARNING|LOG_STDERR, "* Warning: pcap error on setdirection");

    if (pcap == NULL)
    {
        log_msg(LOG_ERR|LOG_STDERR, "[*] pcap error: %s", errstr);
        exit(EXIT_FAILURE);
    }

    /* Set pcap filters, if any. 
    */
    if (opts->config[CONF_PCAP_FILTER] != NULL
      && opts->config[CONF_PCAP_FILTER][0] != '\0')
    {
        if(pcap_compile(pcap, &fp, opts->config[CONF_PCAP_FILTER], 1, 0) == -1)
        {
            log_msg(LOG_ERR|LOG_STDERR, "[*] Error compiling pcap filter: %s",
                pcap_geterr(pcap)
            );
            exit(EXIT_FAILURE);
        }

        if(pcap_setfilter(pcap, &fp) == -1)
        {
            log_msg(LOG_ERR|LOG_STDERR, "[*] Error setting pcap filter: %s",
                pcap_geterr(pcap)
            );
            exit(EXIT_FAILURE);
        }

        log_msg(LOG_INFO|LOG_STDERR, "PCAP filter is: %s", opts->config[CONF_PCAP_FILTER]);

        pcap_freecode(&fp);
    }

    /* Determine and set the data link encapsulation offset.
    */
    switch(pcap_datalink(pcap)) {
        case DLT_EN10MB:
            opts->data_link_offset = 14;
            break;
        case DLT_LINUX_SLL:
            opts->data_link_offset = 16;
            break;
        case DLT_NULL:
            opts->data_link_offset = 4;
            break;
        default:
            opts->data_link_offset = 0;
            break;
    }

    /* Set our pcap handle to nonblocking mode.
    */
    if((pcap_setnonblock(pcap, 1, errstr)) == -1)
    {
        log_msg(LOG_ERR|LOG_STDERR, "[*] Error setting pcap to non-blocking: %s",
            errstr
        );
        exit(EXIT_FAILURE);
    }

    /* Initialize our signal handlers. You can check the return value for
     * the number of signals that were *not* set.  Those that we not set
     * will be listed in the log/stderr output.
    */
    set_sig_handlers();

    log_msg(LOG_INFO|LOG_STDERR, "Starting fwknopd main event loop.");

    /* Jump into our home-grown packet cature loop.
    */
    while(1)
    {
        /* If we got a SIGCHLD and it was the tcp server, then handle it here.
        */
        if(got_sigchld)
        {
            if(opts->tcp_server_pid > 0)
            {
                child_pid = waitpid(0, &status, WNOHANG);

                if(child_pid == opts->tcp_server_pid)
                {
                    if(WIFSIGNALED(status))
                        log_msg(LOG_WARNING, "TCP server got signal: %i",  WTERMSIG(status));

                    log_msg(LOG_WARNING, "TCP server exited with status of %i. Attempting restart.",
                        WEXITSTATUS(status));

                    opts->tcp_server_pid = 0;

                    /* Attempt to restart tcp server ? */
                    usleep(1000000);
                    run_tcp_server(opts);
                }
            }

            got_sigchld = 0;
        }


        /* Any signal except USR1, USR2, and SIGCHLD mean break the loop.
        */
        if(got_signal != 0)
        {
            if(got_sigint || got_sigterm || got_sighup)
            {
                signal(SIGCHLD, SIG_IGN);
                pcap_breakloop(pcap);
                pending_break = 1;
            }
            else if(got_sigusr1 || got_sigusr2)
            {
                /* Not doing anything with these yet.
                */
                got_sigusr1 = got_sigusr2 = 0;
                got_signal = 0;
            }
            else
                got_signal = 0;
        }

        res = pcap_dispatch(pcap, 1, (pcap_handler)&process_packet, (unsigned char *)opts);

        /* If there was a packet and it was processed without error, then
         * keep going.
        */
        if(res > 0 && opts->spa_pkt.packet_data_len > 0)
        {
            res = incoming_spa(opts);

            if(res != 0 && opts->verbose > 1)
            {
                /* make use of the pcap errstr buffer to set the err string
                 * (yeah, I'm lazy).
                */
                if(res == SPA_MSG_NOT_SPA_DATA)
                    strcpy(errstr, "Not SPA Data");
                else if(res == SPA_MSG_LEN_TOO_SMALL)
                    strcpy(errstr, "Msg size too small");
                else if(res == SPA_MSG_HTTP_NOT_ENABLED)
                    strcpy(errstr, "HTTP requests not enabled");
                else
                    strcpy(errstr, "Undefined Error");

                log_msg(LOG_DEBUG, "Got error %i: '%s' on incoming packet.", res, errstr);
            }

            /* Count this packet since it has at least one byte of payload
             * data - we use this as a comparison for --packet-limit regardless
             * of SPA packet validity at this point.
            */
            opts->packet_ctr++;
            if (opts->packet_ctr_limit && opts->packet_ctr >= opts->packet_ctr_limit)
            {
                log_msg(LOG_WARNING|LOG_STDERR,
                    "* Incoming packet count limit of %i reached",
                    opts->packet_ctr_limit
                );

                pcap_breakloop(pcap);
                pending_break = 1;
            }
        }
        /* If there was an error, complain and go on (to an extent before
         * giving up).
        */
        else if(res == -1)
        {
            log_msg(LOG_ERR|LOG_STDERR, "[*] Error from pcap_dispatch: %s",
                pcap_geterr(pcap)
            );

            if(pcap_errcnt++ > MAX_PCAP_ERRORS_BEFORE_BAIL)
            {
                log_msg(LOG_ERR|LOG_STDERR, "[*] %i consecutive pcap errors.  Giving up",
                    pcap_errcnt
                );
                exit(EXIT_FAILURE);
            }
        }
        else if(pending_break == 1 || res == -2)
        {
            /* pcap_breakloop was called, so we bail. */
            log_msg(LOG_INFO|LOG_STDERR, "Gracefully leaving the fwknopd event loop.");
            break;
        }
        else
            pcap_errcnt = 0;

        /* Check for any expired firewall rules and deal with them.
        */
        check_firewall_rules(opts);

        usleep(10000);
    }
#endif /* HAVE_LIBPCAP */

    pcap_close(pcap);

    return(0);
}

/***EOF***/
