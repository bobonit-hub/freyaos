int add(int a, int b)
{
	return a + b;
}

int main(void)
{
	int i, n;

	n = add(20, 22);
	if (n < 0)
		n = -n;
	for (i = 0; i < 3; i++)
		n = n + i;
	return n;
}
