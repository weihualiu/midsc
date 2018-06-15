char *set_sp(char *new_stack)
{
char *sp = new_stack;
	sp += 3;
	return sp;
}

char *restore_sp(char *to,char *from,char *bp,unsigned long size)
{
	*to=from[size];
	size>>=3;
	*bp=3;
	return to-size;
}
