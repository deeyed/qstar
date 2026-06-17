extern int qstar_windows_artifact_plugin(void);

int main(void)
{
	return qstar_windows_artifact_plugin() == 2 ? 0 : 1;
}
