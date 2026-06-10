extern int qstar_embedded_payload_size;

int
main(void)
{
	return qstar_embedded_payload_size > 0 ? 0 : 1;
}
