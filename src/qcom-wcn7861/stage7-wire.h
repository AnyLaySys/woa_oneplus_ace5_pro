/* Wire definitions extracted from the project's ath12k/wmi.h, BSD-3-Clause-Clear.
 * Layouts independently checked against the running PEACH firmware schema. */
#pragma once
#pragma pack(push,1)
#define WMI_MAX_NUM_SS 8
#define WMI_MAX_HECAP_PHY_SIZE (3)
#define WMI_MAX_EHTCAP_MAC_SIZE 2
#define WMI_MAX_EHTCAP_PHY_SIZE 3
#define ATH12K_WMI_SSID_LEN 32
#define ETH_ALEN 6
struct ath12k_wmi_mac_addr_params {
	UCHAR addr[ETH_ALEN];
	UCHAR padding[2];
} ;
struct ath12k_wmi_ssid_params {
	ULONG ssid_len;
	UCHAR ssid[ATH12K_WMI_SSID_LEN];
} ;
struct ath12k_wmi_ppe_threshold_params {
	ULONG numss_m1; /** NSS - 1*/
	ULONG ru_info;
	ULONG ppet16_ppet8_ru3_ru0[WMI_MAX_NUM_SS];
} ;
struct ath12k_wmi_channel_params {
	ULONG tlv_header;
	ULONG mhz;
	ULONG band_center_freq1;
	ULONG band_center_freq2;
	ULONG info;
	ULONG reg_info_1;
	ULONG reg_info_2;
} ;
struct ath12k_wmi_vht_rate_set_params {
	ULONG tlv_header;
	ULONG rx_max_rate;
	/* MCS at which the peer can transmit */
	ULONG rx_mcs_set;
	ULONG tx_max_rate;
	/* MCS at which the peer can receive */
	ULONG tx_mcs_set;
	ULONG tx_max_mcs_nss;
} ;
struct wmi_vdev_start_request_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	ULONG requestor_id;
	ULONG beacon_interval;
	ULONG dtim_period;
	ULONG flags;
	struct ath12k_wmi_ssid_params ssid;
	ULONG bcn_tx_rate;
	ULONG bcn_txpower;
	ULONG num_noa_descriptors;
	ULONG disable_hw_ack;
	ULONG preferred_tx_streams;
	ULONG preferred_rx_streams;
	ULONG he_ops;
	ULONG cac_duration_ms;
	ULONG regdomain;
	ULONG min_data_rate;
	ULONG mbssid_flags; /* uses enum wmi_vdev_mbssid_flags */
	ULONG mbssid_tx_vdev_id;
	ULONG eht_ops;
	ULONG punct_bitmap;
} ;
struct wmi_vdev_up_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	ULONG vdev_assoc_id;
	struct ath12k_wmi_mac_addr_params vdev_bssid;
	struct ath12k_wmi_mac_addr_params tx_vdev_bssid;
	ULONG nontx_profile_idx;
	ULONG nontx_profile_cnt;
} ;
struct wmi_peer_create_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG peer_type;
} ;
struct wmi_peer_delete_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} ;
struct wmi_peer_set_param_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG param_id;
	ULONG param_value;
} ;
struct wmi_peer_reorder_queue_setup_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG tid;
	ULONG queue_ptr_lo;
	ULONG queue_ptr_hi;
	ULONG queue_no;
	ULONG ba_window_size_valid;
	ULONG ba_window_size;
} ;
struct wmi_peer_assoc_complete_cmd {
	ULONG tlv_header;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG vdev_id;
	ULONG peer_new_assoc;
	ULONG peer_associd;
	ULONG peer_flags;
	ULONG peer_caps;
	ULONG peer_listen_intval;
	ULONG peer_ht_caps;
	ULONG peer_max_mpdu;
	ULONG peer_mpdu_density;
	ULONG peer_rate_caps;
	ULONG peer_nss;
	ULONG peer_vht_caps;
	ULONG peer_phymode;
	ULONG peer_ht_info[2];
	ULONG num_peer_legacy_rates;
	ULONG num_peer_ht_rates;
	ULONG peer_bw_rxnss_override;
	struct ath12k_wmi_ppe_threshold_params peer_ppet;
	ULONG peer_he_cap_info;
	ULONG peer_he_ops;
	ULONG peer_he_cap_phy[WMI_MAX_HECAP_PHY_SIZE];
	ULONG peer_he_mcs;
	ULONG peer_he_cap_info_ext;
	ULONG peer_he_cap_info_internal;
	ULONG min_data_rate;
	ULONG peer_he_caps_6ghz;
	ULONG sta_type;
	ULONG bss_max_idle_option;
	ULONG auth_mode;
	ULONG peer_flags_ext;
	ULONG punct_bitmap;
	ULONG peer_eht_cap_mac[WMI_MAX_EHTCAP_MAC_SIZE];
	ULONG peer_eht_cap_phy[WMI_MAX_EHTCAP_PHY_SIZE];
	ULONG peer_eht_ops;
	struct ath12k_wmi_ppe_threshold_params peer_eht_ppet;
} ;
struct wmi_vdev_install_key_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG key_idx;
	ULONG key_flags;
	ULONG key_cipher;
	ULONG64 key_rsc_counter;
	ULONG64 key_global_rsc_counter;
	ULONG64 key_tsc_counter;
	UCHAR wpi_key_rsc_counter[16];
	UCHAR wpi_key_tsc_counter[16];
	ULONG key_len;
	ULONG key_txmic_len;
	ULONG key_rxmic_len;
	ULONG is_group_key_id_valid;
	ULONG group_key_id;

	/* Followed by key_data containing key followed by
	 * tx mic and then rx mic
	 */
} ;
struct wmi_mgmt_send_cmd {
	ULONG tlv_header;
	ULONG vdev_id;
	ULONG desc_id;
	ULONG chanfreq;
	ULONG paddr_lo;
	ULONG paddr_hi;
	ULONG frame_len;
	ULONG buf_len;
	ULONG tx_params_valid;

	/* This TLV is followed by struct wmi_mgmt_frame */

	/* Followed by struct ath12k_wmi_mlo_mgmt_send_params */
} ;
struct wmi_vdev_start_resp_event {
	ULONG vdev_id;
	ULONG requestor_id;
	/* enum wmi_start_event_param */
	ULONG resp_type;
	ULONG status;
	ULONG chain_mask;
	ULONG smps_mode;
	union {
		ULONG mac_id;
		ULONG pdev_id;
	} route;
	ULONG cfgd_tx_streams;
	ULONG cfgd_rx_streams;
	ULONG max_allowed_tx_power;
} ;
struct wmi_vdev_install_key_compl_event {
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	ULONG key_idx;
	ULONG key_flags;
	ULONG status;
} ;
struct wmi_peer_assoc_conf_event {
	ULONG vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} ;
struct wmi_mgmt_tx_compl_event {
	ULONG desc_id;
	ULONG status;
	ULONG pdev_id;
	ULONG ppdu_id;
	ULONG ack_rssi;
} ;
#pragma pack(pop)
#define S7_WMI_VDEV_START_REQUEST_CMDID 0x5003u
#define S7_WMI_VDEV_UP_CMDID 0x5005u
#define S7_WMI_VDEV_STOP_CMDID 0x5006u
#define S7_WMI_PEER_CREATE_CMDID 0x6001u
#define S7_WMI_PEER_DELETE_CMDID 0x6002u
#define S7_WMI_PEER_ASSOC_CMDID 0x6005u
#define S7_WMI_PEER_SET_PARAM_CMDID 0x6004u
#define S7_WMI_PEER_REORDER_QUEUE_SETUP_CMDID 0x6013u
#define S7_WMI_VDEV_INSTALL_KEY_CMDID 0x5009u
#define S7_WMI_MGMT_TX_SEND_CMDID 0x7008u
#define S7_WMI_VDEV_START_RESP_EVENTID 0x5001u
#define S7_WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID 0x5003u
#define S7_WMI_PEER_ASSOC_CONF_EVENTID 0x6006u
#define S7_WMI_MGMT_TX_COMPLETION_EVENTID 0x7006u
#define S7_WMI_VDEV_PARAM_TX_ENCAP_TYPE 0x30u
#define S7_WMI_VDEV_PARAM_RX_DECAP_TYPE 0x5bu
#define S7_WMI_PEER_AUTHORIZE 0x3u
#define S7_WMI_PEER_AUTH 0x1u
#define S7_WMI_PEER_NEED_PTK_4_WAY 0x4u
#define S7_WMI_PEER_NEED_GTK_2_WAY 0x10u
#define S7_WMI_TAG_VDEV_START_REQUEST_CMD 0x58u
#define S7_WMI_TAG_VDEV_UP_CMD 0x5cu
#define S7_WMI_TAG_VDEV_STOP_CMD 0x5du
#define S7_WMI_TAG_PEER_CREATE_CMD 0x61u
#define S7_WMI_TAG_PEER_DELETE_CMD 0x62u
#define S7_WMI_TAG_PEER_ASSOC_COMPLETE_CMD 0x65u
#define S7_WMI_TAG_PEER_SET_PARAM_CMD 0x64u
#define S7_WMI_TAG_REORDER_QUEUE_SETUP_CMD 0x225u
#define S7_WMI_TAG_VDEV_INSTALL_KEY_CMD 0x60u
#define S7_WMI_TAG_MGMT_TX_SEND_CMD 0x1a6u
#define S7_WMI_TAG_VDEV_START_RESPONSE_EVENT 0x28u
#define S7_WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT 0x2au
#define S7_WMI_TAG_PEER_ASSOC_CONF_EVENT 0x1b2u
#define S7_WMI_TAG_MGMT_TX_COMPL_EVENT 0x1a7u
#define S7_WMI_TAG_VHT_RATE_SET 0x66u
