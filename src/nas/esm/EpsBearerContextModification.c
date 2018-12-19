/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under 
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.  
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*****************************************************************************
  Source      EpsBearerContextModification.c

  Version     0.1

  Date        2018/11/19

  Product     NAS stack

  Subsystem   EPS Session Management

  Author      Dincer Beken

  The purpose of the EPS bearer context modification procedure is to modify an EPS bearer context with a specific QoS
  and TFT. The EPS bearer context modification procedure is initiated by the network, but it may also be initiated as part
  of the UE requested bearer resource allocation procedure or the UE requested bearer resource modification procedure.

  The network may also initiate the EPS bearer context modification procedure to update the APN-AMBR of the UE, for
  instance after an inter-system handover. See 3GPP TS 23.401 [10] annex E.

  The network may initiate the EPS bearer context modification procedure together with the completion of the service
  request procedure.

*****************************************************************************/
#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "bstrlib.h"

#include "log.h"
#include "dynamic_memory_check.h"
#include "common_types.h"
#include "assertions.h"
#include "3gpp_24.007.h"
#include "3gpp_24.008.h"
#include "3gpp_29.274.h"
#include "mme_app_ue_context.h"
#include "common_defs.h"
#include "mme_config.h"
#include "nas_itti_messaging.h"
#include "mme_app_defs.h"
#include "emm_data.h"
#include "emm_sap.h"
#include "esm_ebr.h"
#include "esm_proc.h"
#include "esm_cause.h"
#include "nas_esm_proc.h"

/****************************************************************************/
/****************  E X T E R N A L    D E F I N I T I O N S  ****************/
/****************************************************************************/

/****************************************************************************/
/*******************  L O C A L    D E F I N I T I O N S  *******************/
/****************************************************************************/

/*
   --------------------------------------------------------------------------
   Internal data handled by the EPS bearer context modification
   procedure in the MME
   --------------------------------------------------------------------------
*/
/*
   Timer handlers
*/
static int _modify_eps_bearer_context_t3486_handler (nas_esm_proc_bearer_context_t * esm_proc_bearer_context, ESM_msg *esm_resp_msg);

/* Maximum value of the modify EPS bearer context request
   retransmission counter */
#define EPS_BEARER_CONTEXT_MODIFY_COUNTER_MAX   5

/****************************************************************************/
/******************  E X P O R T E D    F U N C T I O N S  ******************/
/****************************************************************************/

/****************************************************************************
 **                                                                        **
 ** Name:    esm_send_modify_eps_bearer_context_request()  **
 **                                                                        **
 ** Description: Builds Modify EPS Bearer Context Request message  **
 **                                                                        **
 **      The modify EPS bearer context request message **
 **      is sent by the network to the UE to request modification of **
 **      an EPS bearer context which is already activated..          **
 **                                                                        **
 ** Inputs:  pti:       Procedure transaction identity             **
 **      ebi:       EPS bearer identity                        **
 **      qos:       EPS quality of service                     **
 **      tft:       Traffic flow template                      **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     msg:       The ESM message to be sent                 **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
void
esm_send_modify_eps_bearer_context_request (
  pti_t pti,
  ebi_t ebi,
  ESM_msg * esm_msg,
  const EpsQualityOfService * qos,
  traffic_flow_template_t *tft,
  ambr_t *ambr,
  protocol_configuration_options_t *pco)
{
  OAILOG_FUNC_IN (LOG_NAS_ESM);

  memset(esm_msg, 0, sizeof(ESM_msg));
  /*
   * Mandatory - ESM message header
   */
  esm_msg->modify_eps_bearer_context_request.protocoldiscriminator = EPS_SESSION_MANAGEMENT_MESSAGE;
  esm_msg->modify_eps_bearer_context_request.epsbeareridentity = ebi;
  esm_msg->modify_eps_bearer_context_request.messagetype = MODIFY_EPS_BEARER_CONTEXT_REQUEST;
  esm_msg->modify_eps_bearer_context_request.proceduretransactionidentity = pti;
  esm_msg->modify_eps_bearer_context_request.presencemask = 0;

  /*
   * Optional - EPS QoS
   */
  if(qos) {
    esm_msg->modify_eps_bearer_context_request.newepsqos = *qos;
    esm_msg->modify_eps_bearer_context_request.presencemask |= MODIFY_EPS_BEARER_CONTEXT_REQUEST_NEW_QOS_PRESENT;
  }
  /*
   * Optional - traffic flow template.
   * Send the TFT as it is. It will contain the operation.
   * We will modify the bearer context when the bearers are accepted.
   */
  if (tft) {
    memcpy(&esm_msg->modify_eps_bearer_context_request.tft, tft, sizeof(traffic_flow_template_t));
    esm_msg->modify_eps_bearer_context_request.presencemask |= MODIFY_EPS_BEARER_CONTEXT_REQUEST_TFT_PRESENT;
  }
  /*
   * Optional - APN AMBR
   * Implementing subscribed values.
   */
  if(ambr){
    if(ambr->br_dl && ambr->br_ul){
      esm_msg->modify_eps_bearer_context_request.presencemask |= MODIFY_EPS_BEARER_CONTEXT_REQUEST_APNAMBR_PRESENT;
      ambr_kbps_calc(&esm_msg->modify_eps_bearer_context_request.apnambr, (ambr->br_dl/1000), (ambr->br_ul/1000));
    }
  }else {
    OAILOG_WARNING (LOG_NAS_ESM, "ESM-SAP   - no APN AMBR is present for activating default eps bearer. \n");
  }

  if (pco) {
    memcpy(&esm_msg->modify_eps_bearer_context_request.protocolconfigurationoptions, pco, sizeof(protocol_configuration_options_t));
    esm_msg->modify_eps_bearer_context_request.presencemask |= MODIFY_EPS_BEARER_CONTEXT_REQUEST_PROTOCOL_CONFIGURATION_OPTIONS_PRESENT;
  }
  OAILOG_INFO (LOG_NAS_ESM, "ESM-SAP   - Send Modify EPS Bearer Context " "Request message (pti=%d, ebi=%d). \n",
      esm_msg->modify_eps_bearer_context_request.proceduretransactionidentity, esm_msg->modify_eps_bearer_context_request.epsbeareridentity);
  OAILOG_FUNC_OUT(LOG_NAS_ESM);
}

/*
   --------------------------------------------------------------------------
      EPS bearer context modification procedure executed by the MME
   --------------------------------------------------------------------------
*/
/****************************************************************************
 **                                                                        **
 ** Name:    esm_proc_modify_eps_bearer_context_accept()                   **
 **                                                                        **
 ** Description: Allocates resources required for modification of an EPS   **
 **      bearer context.                                                   **
 **                                                                        **
 ** Inputs:  ue_id:     ueId                        **
 **          pid:       PDN connection identifier                  **
 **      esm_qos:   EPS bearer level QoS parameters            **
 **      tft:       Traffic flow template parameters           **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     ebi:       EPS bearer identity assigned to the new    **
 **             dedicated bearer context                   **
 **      default_ebi:   EPS bearer identity of the associated de-  **
 **             fault EPS bearer context                   **
 **      esm_cause: Cause code returned upon ESM procedure     **
 **             failure                                    **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
esm_cause_t
esm_proc_modify_eps_bearer_context (
  mme_ue_s1ap_id_t   ue_id,
  const proc_tid_t   pti,
  const ebi_t        linked_ebi,
  const pdn_cid_t    pdn_cid,
  bearer_context_to_be_updated_t * bc_tbu,
  ambr_t             *apn_ambr)
{
  OAILOG_FUNC_IN (LOG_NAS_ESM);
  esm_cause_t                             esm_cause = ESM_CAUSE_SUCCESS;
  int                                     rc = RETURNok;

  OAILOG_INFO (LOG_NAS_ESM, "ESM-PROC  - EPS bearer context modification " "(ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);

  esm_cause = mme_app_esm_modify_bearer_context(ue_id, bc_tbu->eps_bearer_id, bc_tbu->bearer_level_qos, &bc_tbu->tft, apn_ambr);
  if(esm_cause != ESM_CAUSE_SUCCESS){
    OAILOG_ERROR(LOG_NAS_ESM, "ESM-PROC  - Error validating the bearer context modification procedure UE " MME_UE_S1AP_ID_FMT " (ebi=%d). \n", ue_id, bc_tbu->eps_bearer_id);
    OAILOG_FUNC_RETURN(LOG_NAS_ESM, esm_cause);
  }
  /*
   * No need to check for PDN connectivity procedures.
   * They should be handled together.
   * Create a new EPS bearer context transaction and starts the timer, since no further CN operation necessary for dedicated bearers.
   */
  nas_esm_proc_bearer_context_t * esm_proc_bearer_context = _esm_proc_create_bearer_context_procedure(ue_id, pti, linked_ebi, pdn_cid, bc_tbu->eps_bearer_id,
      mme_config.nas_config.t3486_sec, 0 /*usec*/, _modify_eps_bearer_context_t3486_handler);
  if(!esm_proc_bearer_context){
    OAILOG_ERROR(LOG_NAS_ESM, "ESM-PROC  - Error creating a new procedure for the bearer context (ebi=%d) for UE " MME_UE_S1AP_ID_FMT ". \n", bc_tbu->eps_bearer_id, ue_id);
    /* We finalize the bearer context (no modification, just setting back to active mode). */
    mme_app_finalize_bearer_context(ue_id, bc_tbu->eps_bearer_id, NULL, NULL, NULL);
    OAILOG_FUNC_RETURN (LOG_NAS_ESM, ESM_CAUSE_REQUEST_REJECTED_BY_GW);
  }
  /* Verify and set the APN-AMBR in the procedure. */
  esm_proc_bearer_context->apn_ambr.br_dl = apn_ambr->br_dl;
  esm_proc_bearer_context->apn_ambr.br_ul = apn_ambr->br_ul;

  OAILOG_INFO (LOG_NAS_ESM, "ESM-PROC  - Verified EPS bearer context modification " "(ue_id=" MME_UE_S1AP_ID_FMT ", ebi=%d)\n", ue_id, bc_tbu->eps_bearer_id);
  OAILOG_FUNC_RETURN(LOG_NAS_ESM, esm_cause);
}

/****************************************************************************
 **                                                                        **
 ** Name:    esm_proc_modify_eps_bearer_context_accept()            **
 **                                                                        **
 ** Description: Performs EPS bearer context modification procedu- **
 **      re accepted by the UE.                                    **
 **                                                                        **
 **      3GPP TS 24.301, section 6.4.3.3                           **
 **      Upon receipt of the MODIFY EPS BEARER CONTEXT ACCEPT      **
 **      message, the MME shall stop the timer T3486 and enter     **
 **      enter the state BEARER CONTEXT ACTIVE.                    **
 **                                                                        **
 ** Inputs:  ue_id:      UE local identifier                        **
 **      ebi:       EPS bearer identity                        **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     esm_cause: Cause code returned upon ESM procedure     **
 **             failure                                    **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
esm_cause_t
esm_proc_modify_eps_bearer_context_accept (
  mme_ue_s1ap_id_t ue_id,
  ebi_t ebi,
  nas_esm_proc_bearer_context_t *esm_bearer_procedure,
  pti_t pti)
{
  OAILOG_FUNC_IN (LOG_NAS_ESM);
  esm_cause_t                             esm_cause = ESM_CAUSE_SUCCESS;
  int                                     rc = RETURNerror;

  OAILOG_INFO (LOG_NAS_ESM, "ESM-PROC  - EPS bearer context modification " "accepted by the UE (ue_id=" MME_UE_S1AP_ID_FMT ", ebi=%d)\n",
      ue_id, ebi);

  /*
   * Set the MME_APP Bearer context as final.
   */
  esm_cause = mme_app_finalize_bearer_context(ue_id, esm_bearer_procedure->pdn_cid, esm_bearer_procedure->linked_ebi, ebi, &esm_bearer_procedure->apn_ambr, NULL, NULL, NULL);

  /*
   * Stop the ESM procedure.
   */
  _esm_proc_free_bearer_context_procedure(&esm_bearer_procedure);

  if(esm_cause == ESM_CAUSE_SUCCESS){
    /**
     * The ESM procedure was terminated implicitly but the bearer context remained (old status was same (ESM_ACTIVE).
     * We changed the status of the ESM bearer context, so the ESM reply came before any E-RAB failures, if any.
     *
     * The current bearers are not updated yet.
     */
    nas_itti_modify_eps_bearer_ctx_cnf(ue_id, ebi);
  }else {
    /** Bearer not found, assuming due E-RAB error handling. */
    nas_itti_modify_eps_bearer_ctx_rej(ue_id, ebi, esm_cause);
  }

  OAILOG_FUNC_RETURN (LOG_NAS_ESM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    esm_proc_modify_eps_bearer_context_reject()            **
 **                                                                        **
 ** Description: Performs dedicated EPS bearer context modification
 **      procedure not accepted by the UE.                                 **
 **                                                                        **
 **      3GPP TS 24.301, section 6.4.3.4                           **
 **      Upon receipt of the MODIFY EPS BEARER CONTEXT REJECT      **
 **      message, the MME shall stop the timer T3486, enter the    **
 **      state BEARER CONTEXT ACTIVE and abort the EPS EPS bearer context
 **      modification activation procedure.
 **
 **      If the network receives the MODIFY EPS BEARER CONTEXT REJECT      **
 **      message with ESM cause #43 "invalid EPS bearer identity", the MME **
 **      locally deactivates the EPS bearer context(s)                     **
 **      without peer-to-peer ESM signaling.                               **
 **                                                                        **
 ** Inputs:  ue_id:      UE local identifier                        **
 **      ebi:       EPS bearer identity                        **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     esm_cause: Cause code returned upon ESM procedure     **
 **             failure                                    **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
esm_cause_t
esm_proc_modify_eps_bearer_context_reject (
  mme_ue_s1ap_id_t ue_id,
  ebi_t ebi,
  pti_t pti,
  esm_cause_t *esm_cause)
{
  int                                     rc;

  OAILOG_FUNC_IN (LOG_NAS_ESM);
  OAILOG_WARNING (LOG_NAS_ESM, "ESM-PROC  - EPS bearer context modification " "not accepted by the UE (ue_id=" MME_UE_S1AP_ID_FMT ", ebi=%d, esm_cause %d)\n",
      ue_id, ebi, *esm_cause);
  /*
   * Stop T3486 timer if running.
   * Will also check if the bearer exists. If not (E-RAB Setup Failure), the message will be dropped.
   */
  // todo: fix this

  nas_esm_proc_bearer_context_t * esm_proc_bearer_context = _esm_proc_get_bearer_context_procedure(ue_id, pti, ebi);
  if(!esm_proc_bearer_context){
    OAILOG_WARNING (LOG_NAS_ESM, "ESM-PROC  - No EPS bearer context modification procedure exists for pti=%d" "not accepted by the UE (ue_id=" MME_UE_S1AP_ID_FMT ", ebi=%d, cause=%d)\n",
        pti, ue_id, ebi, *esm_cause);
    /** Still send the rejection message. */
  }else {
    _esm_proc_free_bearer_context_procedure(&esm_proc_bearer_context);
  }

  /*
   * Session bearer still exists.
   * Check the cause code and depending continue or disable the bearer context locally.
   */

  esm_ebr_state oldState = _esm_ebr_get_status(ue_id, ebi);

  if(*esm_cause == ESM_CAUSE_INVALID_EPS_BEARER_IDENTITY){
      OAILOG_WARNING (LOG_NAS_ESM, "ESM-PROC  - Deactivating the ESM bearer locally. " "(ue_id=" MME_UE_S1AP_ID_FMT ", ebi=%d)\n", ue_id, ebi);
      ebi = mme_app_release_bearer_context (ue_id, 0, ebi);
      if (ebi == ESM_EBI_UNASSIGNED) {  /**< Bearer was already released. */
        OAILOG_WARNING (LOG_NAS_ESM, "ESM-PROC  - Failed to release EPS bearer context\n");
        OAILOG_FUNC_RETURN (LOG_NAS_ESM, rc);
      }
  }

  if (*esm_cause != ESM_CAUSE_INVALID_EPS_BEARER_IDENTITY) {
      /*
       * Session bearer existed and was not removed.
       * Failed to release the modify the EPS bearer context
       */
      *esm_cause = ESM_CAUSE_PROTOCOL_ERROR;
      /** Set the state back to active. */
//      esm_ebr_set_status(esm_context, ebi, ESM_EBR_ACTIVE, ue_requested);
    }
    /**
     * Inform the MME_APP only if it came as an ESM response, the bearer existed and no E-RAB failure was received yet. The S11 procedure should be handled with already.
     * We don't check the return value of status setting, because the bearer might have also been removed implicitly.
     */
    if(oldState != ESM_EBR_ACTIVE) /**< Check if the procedure was not released before due E-RAB failure. */
      nas_itti_modify_eps_bearer_ctx_rej(ue_id, ebi, esm_cause); /**< The SAE-GW should release the cause, too. */
//  } else {
//    /** No E-RAB  could be found. Can live with it. */
//  }

  OAILOG_FUNC_RETURN (LOG_NAS_ESM, rc);
}

/****************************************************************************/
/*********************  L O C A L    F U N C T I O N S  *********************/
/****************************************************************************/

/*
   --------------------------------------------------------------------------
                Timer handlers
   --------------------------------------------------------------------------
*/
/****************************************************************************
 **                                                                        **
 ** Name:    _modify_eps_bearer_context_t3486_handler()                    **
 **                                                                        **
 ** Description: T3486 timeout handler                                     **
 **                                                                        **
 **              3GPP TS 24.301, section 6.4.3.6, case a                   **
 **      On the first expiry of the timer T3486, the MME shall re- **
 **      send the MODIFY DEDICATED EPS BEARER CONTEXT REQUEST    **
 **      and shall reset and restart timer T3486. This retransmis- **
 **      sion is repeated four times, i.e. on the fifth expiry of  **
 **      timer T3486, the MME shall abort the procedure, release   **
 **      any resources allocated for this activation and enter the **
 **      state BEARER CONTEXT ACTIVE .
 **
 **      The MME may continue to use the previous configuration of **
 **      the EPS bearer context or initiate an EPS bearer context  **
 **      deactivation procedure (currently continue using          **
 **      previous configuration).                                  **
 **
 **                                                                        **
 ** Inputs:  args:      handler parameters                         **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    None                                       **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
static int _modify_eps_bearer_context_t3486_handler (nas_esm_proc_bearer_context_t * esm_proc_bearer_context, ESM_msg *esm_resp_msg)
{
  OAILOG_FUNC_IN (LOG_NAS_ESM);
  int                                     rc;

  esm_proc_bearer_context->esm_base_proc.retx_count += 1;
  if (esm_proc_bearer_context->esm_base_proc.retx_count < EPS_BEARER_CONTEXT_MODIFY_COUNTER_MAX) {
    /*
     * Create a new ESM-Information request and restart the timer.
     * Keep the ESM transaction.
     */

    pdn_context_t * pdn_context = NULL;
    mme_app_get_pdn_context(esm_proc_bearer_context->esm_base_proc.ue_id, esm_proc_bearer_context->pdn_cid,
        esm_proc_bearer_context->linked_ebi, NULL , &pdn_context);
    if(pdn_context){
      bearer_context_t * bearer_context = NULL;
      mme_app_get_session_bearer_context(pdn_context, esm_proc_bearer_context->bearer_ebi);
      if(bearer_context){
        /* Resend the message and restart the timer. */
        EpsQualityOfService eps_qos = {0};
         /** Sending a EBR-Request per bearer context. */
        memset((void*)&eps_qos, 0, sizeof(eps_qos));
        /** Set the EPS QoS. */
        qos_params_to_eps_qos(bearer_context->bearer_level_qos.qci,
            bearer_context->bearer_level_qos.mbr.br_dl, bearer_context->bearer_level_qos.mbr.br_ul,
            bearer_context->bearer_level_qos.gbr.br_dl, bearer_context->bearer_level_qos.gbr.br_ul,
            &eps_qos, false);
        esm_send_modify_eps_bearer_context_request(esm_proc_bearer_context->esm_base_proc.pti,
             bearer_context->ebi,
             esm_resp_msg,
             &bearer_context->bearer_level_qos,
             bearer_context->esm_ebr_context.tft,
             &esm_proc_bearer_context->apn_ambr,
             /* bearer_context->esm_ebr_context.pco */ NULL);
        // todo: ideally, the following should also be in a lock..
        /* Restart the T3486 timer. */
        nas_stop_esm_timer(esm_proc_bearer_context->esm_base_proc.ue_id, &esm_proc_bearer_context->esm_base_proc.esm_proc_timer);
        esm_proc_bearer_context->esm_base_proc.esm_proc_timer.id = nas_esm_timer_start(mme_config.nas_config.t3486_sec, 0 /*usec*/, (void*)esm_proc_bearer_context->esm_base_proc.ue_id);
        esm_proc_bearer_context->esm_base_proc.timeout_notif = _modify_eps_bearer_context_t3486_handler;
        OAILOG_FUNC_RETURN(LOG_NAS_ESM, rc);
       }
     }
   }
   /*
    * The maximum number of activate dedicated EPS bearer context request
    * message retransmission has exceed
    */
   pdn_cid_t                               pid = MAX_APN_PER_UE;
   /*
    * Finalize the bearer (set to ESM_EBR_STATE ACTIVE).
    */
   /** Send a reject back to the MME_APP layer. */
   nas_itti_modify_eps_bearer_ctx_rej(esm_proc_bearer_context->esm_base_proc.ue_id, esm_proc_bearer_context->bearer_ebi, ESM_CAUSE_REQUEST_REJECTED_BY_GW);
   /*
    * Release the transactional PDN connectivity procedure.
    */
   _esm_proc_free_bearer_context_procedure(&esm_proc_bearer_context);
   /*
    * Release the dedicated EPS bearer context and enter state INACTIVE
    */
   mme_app_finalize_bearer_context(esm_proc_bearer_context->esm_base_proc.ue_id, esm_proc_bearer_context->bearer_ebi, NULL, NULL, NULL);
   OAILOG_FUNC_OUT (LOG_NAS_ESM);
}

/*
   --------------------------------------------------------------------------
                MME specific local functions
   --------------------------------------------------------------------------
*/